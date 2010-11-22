/*
 * Copyright (C) 2007 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER );

/**
 * @file
 *
 * iPXE scripts
 *
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <ipxe/command.h>
#include <ipxe/parseopt.h>
#include <ipxe/image.h>

struct image_type script_image_type __image_type ( PROBE_NORMAL );

/** Currently running script
 *
 * This is a global in order to allow goto_exec() to update the
 * offset.
 */
static struct image *script;

/** Offset within current script
 *
 * This is a global in order to allow goto_exec() to update the
 * offset.
 */
static size_t script_offset;

/**
 * Process script lines
 *
 * @v process_line	Line processor
 * @v terminate		Termination check
 * @ret rc		Return status code
 */
static int process_script ( int ( * process_line ) ( const char *line ),
			    int ( * terminate ) ( int rc ) ) {
	off_t eol;
	size_t len;
	int rc;

	script_offset = 0;

	do {
	
		/* Find length of next line, excluding any terminating '\n' */
		eol = memchr_user ( script->data, script_offset, '\n',
				    ( script->len - script_offset ) );
		if ( eol < 0 )
			eol = script->len;
		len = ( eol - script_offset );

		/* Copy line, terminate with NUL, and execute command */
		{
			char cmdbuf[ len + 1 ];

			copy_from_user ( cmdbuf, script->data,
					 script_offset, len );
			cmdbuf[len] = '\0';
			DBG ( "$ %s\n", cmdbuf );

			/* Move to next line */
			script_offset += ( len + 1 );

			/* Process line */
			rc = process_line ( cmdbuf );
			if ( terminate ( rc ) )
				return rc;
		}

	} while ( script_offset < script->len );

	return rc;
}

/**
 * Terminate script processing if line processing failed
 *
 * @v rc		Line processing status
 * @ret terminate	Terminate script processing
 */
static int terminate_on_failure ( int rc ) {
	return ( rc != 0 );
}

/**
 * Terminate script processing if line processing succeeded
 *
 * @v rc		Line processing status
 * @ret terminate	Terminate script processing
 */
static int terminate_on_success ( int rc ) {
	return ( rc == 0 );
}

/**
 * Execute script line
 *
 * @v line		Line of script
 * @ret rc		Return status code
 */
static int script_exec_line ( const char *line ) {
	int rc;

	/* Skip label lines */
	if ( line[0] == ':' )
		return 0;

	/* Execute command */
	if ( ( rc = system ( line ) ) != 0 ) {
		printf ( "Aborting on \"%s\"\n", line );
		return rc;
	}

	return 0;
}

/**
 * Execute script
 *
 * @v image		Script
 * @ret rc		Return status code
 */
static int script_exec ( struct image *image ) {
	struct image *saved_script;
	size_t saved_offset;
	int rc;

	/* Temporarily de-register image, so that a "boot" command
	 * doesn't throw us into an execution loop.
	 */
	unregister_image ( image );

	/* Preserve state of any currently-running script */
	saved_script = script;
	saved_offset = script_offset;

	/* Initialise state for this script */
	script = image;

	/* Process script */
	rc = process_script ( script_exec_line, terminate_on_failure );

	/* Restore saved state, re-register image, and return */
	script_offset = saved_offset;
	script = saved_script;
	register_image ( image );
	return rc;
}

/**
 * Load script into memory
 *
 * @v image		Script
 * @ret rc		Return status code
 */
static int script_load ( struct image *image ) {
	static const char ipxe_magic[] = "#!ipxe";
	static const char gpxe_magic[] = "#!gpxe";
	linker_assert ( sizeof ( ipxe_magic ) == sizeof ( gpxe_magic ),
			magic_size_mismatch );
	char test[ sizeof ( ipxe_magic ) - 1 /* NUL */
		   + 1 /* terminating space */];

	/* Sanity check */
	if ( image->len < sizeof ( test ) ) {
		DBG ( "Too short to be a script\n" );
		return -ENOEXEC;
	}

	/* Check for magic signature */
	copy_from_user ( test, image->data, 0, sizeof ( test ) );
	if ( ! ( ( ( memcmp ( test, ipxe_magic, sizeof ( test ) - 1 ) == 0 ) ||
		   ( memcmp ( test, gpxe_magic, sizeof ( test ) - 1 ) == 0 )) &&
		 isspace ( test[ sizeof ( test ) - 1 ] ) ) ) {
		DBG ( "Invalid magic signature\n" );
		return -ENOEXEC;
	}

	/* This is a script */
	image->type = &script_image_type;

	/* We don't actually load it anywhere; we will pick the lines
	 * out of the image as we need them.
	 */

	return 0;
}

/** Script image type */
struct image_type script_image_type __image_type ( PROBE_NORMAL ) = {
	.name = "script",
	.load = script_load,
	.exec = script_exec,
};

/** "goto" options */
struct goto_options {};

/** "goto" option list */
static struct option_descriptor goto_opts[] = {};

/** "goto" command descriptor */
static struct command_descriptor goto_cmd =
	COMMAND_DESC ( struct goto_options, goto_opts, 1, 1,
		       "<label>", "" );

/**
 * Current "goto" label
 *
 * Valid only during goto_exec().  Consider this part of a closure.
 */
static const char *goto_label;

/**
 * Check for presence of label
 *
 * @v line		Script line
 * @ret rc		Return status code
 */
static int goto_find_label ( const char *line ) {

	if ( line[0] != ':' )
		return -ENOENT;
	if ( strcmp ( goto_label, &line[1] ) != 0 )
		return -ENOENT;
	return 0;
}

/**
 * "goto" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int goto_exec ( int argc, char **argv ) {
	struct goto_options opts;
	size_t saved_offset;
	int rc;

	/* Parse options */
	if ( ( rc = parse_options ( argc, argv, &goto_cmd, &opts ) ) != 0 )
		return rc;

	/* Sanity check */
	if ( ! script ) {
		printf ( "Not in a script\n" );
		return -ENOTTY;
	}

	/* Parse label */
	goto_label = argv[optind];

	/* Find label */
	saved_offset = script_offset;
	if ( ( rc = process_script ( goto_find_label,
				     terminate_on_success ) ) != 0 ) {
		script_offset = saved_offset;
		return rc;
	}

	return 0;
}

/** "goto" command */
struct command goto_command __command = {
	.name = "goto",
	.exec = goto_exec,
};
