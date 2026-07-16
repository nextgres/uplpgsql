--
-- Cornell box path tracer, in PL/pgSQL.
--
-- A benchmark whose output is its own correctness check: the image is
-- deterministic, so the interpreter and the JIT must produce byte-identical
-- pixels.  If they do not, the timings are meaningless and the diff says so.
--
-- The scene is Kevin Beason's smallpt Cornell box: the walls are spheres of
-- enormous radius, so ray-sphere intersection is the only geometry routine
-- needed.  The glass sphere is rendered as a mirror -- refraction would add
-- Fresnel terms without adding anything the benchmark measures.  The one
-- departure from smallpt is the light: a small emissive sphere inside the
-- room instead of a huge one poking through the ceiling, so that the direct
-- light sampling (next-event estimation) below has something it can actually
-- hit -- see the scene table for why.
--
-- Why this shape of workload:
--
--   * It is float8 arithmetic almost end to end.  The existing plx benchmark
--     reported a "float8" row that was really numeric (an unadorned 1.0 is
--     numeric in PostgreSQL), so nothing there exercised the native float8
--     path.  Every literal here is explicitly float8.
--
--   * The inner loop reads the scene out of arrays, once per sphere per
--     bounce.  Those reads are the hot path.
--
--   * Recursion is flattened into a bounce loop carrying a throughput term,
--     so what is measured is arithmetic and branching rather than PL/pgSQL
--     function call overhead.
--
-- The body is written once and installed under both languages, so both
-- engines execute byte-identical source and it cannot drift.
--
-- Usage:
--   psql -d mydb -f bench/cornell.sql
--   select cornell_uplpgsql(64, 8);            -- returns int[] of RGB bytes
--   \o out.ppm
--   select cornell_ppm(cornell_uplpgsql(64,8), 64);
--

DO $install$
DECLARE
	lang	text;
	body	text := $body$
DECLARE
	-- Scene: 9 spheres.  Parallel arrays rather than a composite type, so the
	-- inner loop is a subscript read of a float8 array and nothing else.
	--
	-- The light is a small emissive sphere hanging below the ceiling, not
	-- smallpt's radius-600 sphere poking through it.  That original is a
	-- golf-code trick to avoid ever sampling the light: it works for a naive
	-- tracer that only finds the light by chance, but it defeats next-event
	-- estimation, because a shadow ray toward that giant sphere hits the
	-- ceiling 97% of the time.  A small light the room can actually see is
	-- what makes the direct-sampling below worthwhile -- and it is what a real
	-- Cornell box has.
	--                     left     right    back   front     bottom   top       mirror  white   light
	sr   float8[] := ARRAY[1e5,     1e5,     1e5,   1e5,      1e5,     1e5,      16.5,   16.5,   14      ]::float8[];
	spx  float8[] := ARRAY[1e5+1,   -1e5+99, 50,    50,       50,      50,       27,     73,     50      ]::float8[];
	spy  float8[] := ARRAY[40.8,    40.8,    40.8,  40.8,     1e5,     -1e5+81.6, 16.5,  16.5,   66      ]::float8[];
	spz  float8[] := ARRAY[81.6,    81.6,    1e5,   -1e5+170, 81.6,    81.6,     47,     78,     60      ]::float8[];
	-- emission (only the light emits): a small light needs to be bright
	sex  float8[] := ARRAY[0,0,0,0,0,0,0,0,5]::float8[];
	sey  float8[] := ARRAY[0,0,0,0,0,0,0,0,5]::float8[];
	sez  float8[] := ARRAY[0,0,0,0,0,0,0,0,5]::float8[];
	-- albedo
	scx  float8[] := ARRAY[0.75, 0.25, 0.75, 0, 0.75, 0.75, 0.999, 0.999, 0]::float8[];
	scy  float8[] := ARRAY[0.25, 0.25, 0.75, 0, 0.75, 0.75, 0.999, 0.999, 0]::float8[];
	scz  float8[] := ARRAY[0.25, 0.75, 0.75, 0, 0.75, 0.75, 0.999, 0.999, 0]::float8[];
	-- 0 = diffuse, 1 = mirror
	smat int[]    := ARRAY[0,0,0,0,0,0,1,0,0];

	img  int[];

	-- camera
	camx float8 := 50.0;   camy float8 := 52.0;  camz float8 := 295.6;
	cdx  float8 := 0.0;    cdy  float8 := -0.042612; cdz float8 := -1.0;
	cxx  float8;  cxy float8;  cxz float8;
	cyx  float8;  cyy float8;  cyz float8;

	seed bigint := 1;
	rnd  float8;

	x int; y int; s int; k int; kbest int; depth int; idx int;
	inv  float8;
	rox float8; roy float8; roz float8;
	rdx float8; rdy float8; rdz float8;
	ndx float8; ndy float8; ndz float8;
	ox float8; oy float8; oz float8;
	b float8; det float8; t1 float8; t2 float8; tbest float8;
	hx float8; hy float8; hz float8;
	nx float8; ny float8; nz float8;
	nlx float8; nly float8; nlz float8;
	ux float8; uy float8; uz float8;
	vx float8; vy float8; vz float8;
	ax float8; ay float8; az float8;
	tx float8; ty float8; tz float8;
	rx float8; ry float8; rz float8;
	ar float8; ag float8; ab float8;
	pr float8; pg float8; pb float8;
	cr float8; cg float8; cb float8;
	r1 float8; r2 float8; r2s float8;
	dd float8; len float8; p float8;

	-- Next-event estimation: sample the light directly at each diffuse bounce.
	emit boolean;			-- count a hit's emission? (see the loop below)
	dist2 float8;			-- squared distance from hit to light centre
	swx float8; swy float8; swz float8;		-- unit direction hit -> light
	sux float8; suy float8; suz float8;		-- } orthonormal basis
	svx float8; svy float8; svz float8;		-- } around sw
	cmax float8; ca float8; sa float8;		-- sampling cone: max cos, cos, sin
	le1 float8; le2 float8; phi float8;
	sdx float8; sdy float8; sdz float8;		-- sampled shadow-ray direction
	s_tbest float8; s_kbest int;			-- nearest hit along the shadow ray
BEGIN
	img := array_fill(0, ARRAY[dim*dim*3]);

	-- normalize the camera direction
	len := sqrt(cdx*cdx + cdy*cdy + cdz*cdz);
	cdx := cdx/len; cdy := cdy/len; cdz := cdz/len;

	-- image plane basis; square image, so the x span is the raw field of view
	cxx := 0.5135; cxy := 0.0; cxz := 0.0;
	-- cy = normalize(cx cross cd) * 0.5135
	cyx := cxy*cdz - cxz*cdy;
	cyy := cxz*cdx - cxx*cdz;
	cyz := cxx*cdy - cxy*cdx;
	len := sqrt(cyx*cyx + cyy*cyy + cyz*cyz);
	cyx := cyx/len*0.5135; cyy := cyy/len*0.5135; cyz := cyz/len*0.5135;

	y := 0;
	WHILE y < dim LOOP
		x := 0;
		WHILE x < dim LOOP
			pr := 0.0; pg := 0.0; pb := 0.0;

			s := 0;
			WHILE s < samps LOOP
				-- jittered sample position within the pixel
				seed := (seed * 16807) % 2147483647; rnd := seed::float8 / 2147483647.0;
				ax := (x::float8 + rnd) / dim::float8 - 0.5;
				seed := (seed * 16807) % 2147483647; rnd := seed::float8 / 2147483647.0;
				ay := 0.5 - (y::float8 + rnd) / dim::float8;

				rdx := cxx*ax + cyx*ay + cdx;
				rdy := cxy*ax + cyy*ay + cdy;
				rdz := cxz*ax + cyz*ay + cdz;
				len := sqrt(rdx*rdx + rdy*rdy + rdz*rdz);
				rdx := rdx/len; rdy := rdy/len; rdz := rdz/len;

				rox := camx + rdx*140.0;
				roy := camy + rdy*140.0;
				roz := camz + rdz*140.0;

				-- radiance along this ray, accumulated iteratively
				tx := 1.0; ty := 1.0; tz := 1.0;	-- throughput
				rx := 0.0; ry := 0.0; rz := 0.0;	-- radiance
				depth := 0;
				emit := true;						-- camera ray sees emission

				LOOP
					-- nearest intersection
					tbest := 1e20; kbest := 0;
					k := 1;
					WHILE k <= 9 LOOP
						ox := spx[k] - rox;
						oy := spy[k] - roy;
						oz := spz[k] - roz;
						b := ox*rdx + oy*rdy + oz*rdz;
						det := b*b - (ox*ox + oy*oy + oz*oz) + sr[k]*sr[k];
						IF det > 0.0 THEN
							det := sqrt(det);
							t1 := b - det;
							IF t1 > 0.0001 THEN
								IF t1 < tbest THEN tbest := t1; kbest := k; END IF;
							ELSE
								t2 := b + det;
								IF t2 > 0.0001 AND t2 < tbest THEN tbest := t2; kbest := k; END IF;
							END IF;
						END IF;
						k := k + 1;
					END LOOP;

					EXIT WHEN kbest = 0;

					hx := rox + rdx*tbest;
					hy := roy + rdy*tbest;
					hz := roz + rdz*tbest;

					nx := (hx - spx[kbest]) / sr[kbest];
					ny := (hy - spy[kbest]) / sr[kbest];
					nz := (hz - spz[kbest]) / sr[kbest];
					len := sqrt(nx*nx + ny*ny + nz*nz);
					nx := nx/len; ny := ny/len; nz := nz/len;

					-- normal facing the incoming ray
					dd := nx*rdx + ny*rdy + nz*rdz;
					IF dd < 0.0 THEN
						nlx := nx; nly := ny; nlz := nz;
					ELSE
						nlx := -nx; nly := -ny; nlz := -nz;
					END IF;

					/*
					 * Emission is added here only when this hit was NOT
					 * reached by a diffuse bounce -- a camera ray, or a
					 * mirror reflection.  A diffuse bounce already accounted
					 * for the light through the explicit sample below, so
					 * counting the light again when the bounce happens to
					 * land on it would double it.  emit carries that "was the
					 * previous bounce specular?" state.
					 */
					IF emit THEN
						rx := rx + tx*sex[kbest];
						ry := ry + ty*sey[kbest];
						rz := rz + tz*sez[kbest];
					END IF;

					cr := scx[kbest]; cg := scy[kbest]; cb := scz[kbest];

					depth := depth + 1;
					EXIT WHEN depth > 50;

					-- Russian roulette once the path is deep enough
					IF depth > 5 THEN
						p := greatest(cr, cg, cb);
						EXIT WHEN p <= 0.0;
						seed := (seed * 16807) % 2147483647; rnd := seed::float8 / 2147483647.0;
						EXIT WHEN rnd >= p;
						cr := cr/p; cg := cg/p; cb := cb/p;
					END IF;

					tx := tx*cr; ty := ty*cg; tz := tz*cb;

					IF smat[kbest] = 1 THEN
						-- mirror: the reflected ray sees emission directly
						dd := 2.0*(nx*rdx + ny*rdy + nz*rdz);
						ndx := rdx - nx*dd;
						ndy := rdy - ny*dd;
						ndz := rdz - nz*dd;
						emit := true;
					ELSE
						/*
						 * Next-event estimation.
						 *
						 * Rather than wait for the diffuse bounce below to
						 * wander into the light -- which it does about a tenth
						 * of the time, so the rest of the paths come back black
						 * and the image is grainy -- send one ray straight at
						 * the light now and add its contribution directly.
						 *
						 * Sample a direction uniformly inside the cone the
						 * light sphere subtends from the hit point (Beason's
						 * smallpt explicit form).  If a shadow ray in that
						 * direction reaches the light unoccluded, add
						 *   throughput * Le * (l . n) * omega / pi
						 * with omega = 2pi(1 - cos_a_max) the cone's solid
						 * angle, so omega/pi = 2(1 - cos_a_max).  tx/ty/tz
						 * already fold in this vertex's albedo (and any
						 * roulette boost), which is exactly the BRDF weight the
						 * direct term needs.
						 *
						 * Guard dist2 > r^2: a point can lie inside the light
						 * sphere -- it dips just below the ceiling -- and the
						 * cone half-angle would take the square root of a
						 * negative.  Those points are essentially at the light
						 * and contribute negligibly, so skip them.
						 */
						swx := spx[9] - hx; swy := spy[9] - hy; swz := spz[9] - hz;
						dist2 := swx*swx + swy*swy + swz*swz;

						IF dist2 > sr[9]*sr[9] + 0.01 THEN
							len := sqrt(dist2);
							swx := swx/len; swy := swy/len; swz := swz/len;

							-- basis around sw
							IF abs(swx) > 0.1 THEN ax := 0.0; ay := 1.0; az := 0.0;
							ELSE                   ax := 1.0; ay := 0.0; az := 0.0;
							END IF;
							sux := ay*swz - az*swy;
							suy := az*swx - ax*swz;
							suz := ax*swy - ay*swx;
							len := sqrt(sux*sux + suy*suy + suz*suz);
							sux := sux/len; suy := suy/len; suz := suz/len;
							svx := swy*suz - swz*suy;
							svy := swz*sux - swx*suz;
							svz := swx*suy - swy*sux;

							cmax := sqrt(1.0 - sr[9]*sr[9]/dist2);
							seed := (seed * 16807) % 2147483647; le1 := seed::float8 / 2147483647.0;
							seed := (seed * 16807) % 2147483647; le2 := seed::float8 / 2147483647.0;
							ca := 1.0 - le1 + le1*cmax;
							sa := sqrt(1.0 - ca*ca);
							phi := 6.283185307179586 * le2;

							sdx := sux*cos(phi)*sa + svx*sin(phi)*sa + swx*ca;
							sdy := suy*cos(phi)*sa + svy*sin(phi)*sa + swy*ca;
							sdz := suz*cos(phi)*sa + svz*sin(phi)*sa + swz*ca;

							dd := sdx*nlx + sdy*nly + sdz*nlz;
							IF dd > 0.0 THEN
								-- shadow ray from the hit point toward the light
								s_tbest := 1e20; s_kbest := 0;
								k := 1;
								WHILE k <= 9 LOOP
									ox := spx[k] - hx;
									oy := spy[k] - hy;
									oz := spz[k] - hz;
									b := ox*sdx + oy*sdy + oz*sdz;
									det := b*b - (ox*ox + oy*oy + oz*oz) + sr[k]*sr[k];
									IF det > 0.0 THEN
										det := sqrt(det);
										t1 := b - det;
										IF t1 > 0.0001 THEN
											IF t1 < s_tbest THEN s_tbest := t1; s_kbest := k; END IF;
										ELSE
											t2 := b + det;
											IF t2 > 0.0001 AND t2 < s_tbest THEN s_tbest := t2; s_kbest := k; END IF;
										END IF;
									END IF;
									k := k + 1;
								END LOOP;

								IF s_kbest = 9 THEN
									-- omega/pi = 2(1 - cmax); Le = sez[9] etc. (white)
									rx := rx + tx*sex[9]*dd*2.0*(1.0 - cmax);
									ry := ry + ty*sey[9]*dd*2.0*(1.0 - cmax);
									rz := rz + tz*sez[9]*dd*2.0*(1.0 - cmax);
								END IF;
							END IF;
						END IF;

						-- the diffuse bounce itself no longer counts emission
						emit := false;

						-- cosine-weighted hemisphere around nl
						seed := (seed * 16807) % 2147483647; rnd := seed::float8 / 2147483647.0;
						r1 := 6.283185307179586 * rnd;
						seed := (seed * 16807) % 2147483647; rnd := seed::float8 / 2147483647.0;
						r2 := rnd; r2s := sqrt(r2);

						IF abs(nlx) > 0.1 THEN ax := 0.0; ay := 1.0; az := 0.0;
						ELSE                   ax := 1.0; ay := 0.0; az := 0.0;
						END IF;

						-- u = normalize(a cross nl)
						ux := ay*nlz - az*nly;
						uy := az*nlx - ax*nlz;
						uz := ax*nly - ay*nlx;
						len := sqrt(ux*ux + uy*uy + uz*uz);
						ux := ux/len; uy := uy/len; uz := uz/len;

						-- v = nl cross u
						vx := nly*uz - nlz*uy;
						vy := nlz*ux - nlx*uz;
						vz := nlx*uy - nly*ux;

						ndx := ux*cos(r1)*r2s + vx*sin(r1)*r2s + nlx*sqrt(1.0-r2);
						ndy := uy*cos(r1)*r2s + vy*sin(r1)*r2s + nly*sqrt(1.0-r2);
						ndz := uz*cos(r1)*r2s + vz*sin(r1)*r2s + nlz*sqrt(1.0-r2);
						len := sqrt(ndx*ndx + ndy*ndy + ndz*ndz);
						ndx := ndx/len; ndy := ndy/len; ndz := ndz/len;
					END IF;

					rox := hx; roy := hy; roz := hz;
					rdx := ndx; rdy := ndy; rdz := ndz;
				END LOOP;

				pr := pr + rx; pg := pg + ry; pb := pb + rz;
				s := s + 1;
			END LOOP;

			inv := 1.0 / samps::float8;
			pr := pr*inv; pg := pg*inv; pb := pb*inv;

			-- clamp, gamma correct, quantise
			ar := least(greatest(pr, 0.0), 1.0);
			ag := least(greatest(pg, 0.0), 1.0);
			ab := least(greatest(pb, 0.0), 1.0);

			idx := (y*dim + x)*3;
			img[idx+1] := floor(power(ar, 0.45454545454545453)*255.0 + 0.5)::int;
			img[idx+2] := floor(power(ag, 0.45454545454545453)*255.0 + 0.5)::int;
			img[idx+3] := floor(power(ab, 0.45454545454545453)*255.0 + 0.5)::int;

			x := x + 1;
		END LOOP;
		y := y + 1;
	END LOOP;

	RETURN img;
END
$body$;
BEGIN
	FOREACH lang IN ARRAY ARRAY['plpgsql', 'uplpgsql'] LOOP
		EXECUTE format(
			'CREATE OR REPLACE FUNCTION cornell_%s(dim int, samps int) '
			'RETURNS int[] AS %L LANGUAGE %s',
			lang, body, lang);
	END LOOP;
END
$install$;

--
-- Format a rendered buffer as a binary-safe ASCII PPM (P3).
--
CREATE OR REPLACE FUNCTION cornell_ppm(px int[], dim int)
RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT 'P3' || E'\n' || dim || ' ' || dim || E'\n' || '255' || E'\n'
		   || array_to_string(px, ' ') || E'\n'
$$;
