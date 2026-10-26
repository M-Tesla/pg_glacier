CREATE FUNCTION glacier_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME', 'glacier_fdw_handler'
LANGUAGE C STRICT;

CREATE FUNCTION glacier_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME', 'glacier_fdw_validator'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER glacier_wrapper
HANDLER glacier_fdw_handler
VALIDATOR glacier_fdw_validator;
