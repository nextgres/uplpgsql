/* uplpgsql--1.0.sql */

-- Complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION uplpgsql" to load this file. \quit

CREATE FUNCTION uplpgsql_call_handler()
RETURNS language_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION uplpgsql_inline_handler(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION uplpgsql_validator(oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE TRUSTED LANGUAGE uplpgsql
    HANDLER uplpgsql_call_handler
    INLINE uplpgsql_inline_handler
    VALIDATOR uplpgsql_validator;

COMMENT ON LANGUAGE uplpgsql IS 'LLVM JIT-compiled PL/pgSQL';
