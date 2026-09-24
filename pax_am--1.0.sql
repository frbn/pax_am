/* pax/pax--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pax_am" to load this file. \quit

CREATE FUNCTION pax_tableam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'pax_tableam_handler'
LANGUAGE C STRICT;

-- Access methods
CREATE ACCESS METHOD pax TYPE TABLE HANDLER pax_tableam_handler;
COMMENT ON ACCESS METHOD pax IS
'table AM for Partition Attributes Across data organization model';
