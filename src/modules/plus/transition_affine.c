/*
 * transition_affine.c -- affine transformations
 * Copyright (C) 2003-2004 Ushodaya Enterprises Limited
 * Author: Charles Yates <charles.yates@pandora.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "transition_affine.h"
#include <framework/mlt.h>

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <math.h>

/** Geometry struct.
*/

struct geometry_s
{
	int frame;
	float position;
	float mix;
	int nw; // normalised width
	int nh; // normalised height
	int sw; // scaled width, not including consumer scale based upon w/nw
	int sh; // scaled height, not including consumer scale based upon h/nh
	float x;
	float y;
	float w;
	float h;
	struct geometry_s *next;
};

/** Parse a value from a geometry string.
*/

static float parse_value( char **ptr, int normalisation, char delim, float defaults )
{
	float value = defaults;

	if ( *ptr != NULL && **ptr != '\0' )
	{
		char *end = NULL;
		value = strtod( *ptr, &end );
		if ( end != NULL )
		{
			if ( *end == '%' )
				value = ( value / 100.0 ) * normalisation;
			while ( *end == delim || *end == '%' )
				end ++;
		}
		*ptr = end;
	}

	return value;
}

/** Parse a geometry property string with the syntax X,Y:WxH:MIX. Any value can be 
	expressed as a percentage by appending a % after the value, otherwise values are
	assumed to be relative to the normalised dimensions of the consumer.
*/

static void geometry_parse( struct geometry_s *geometry, struct geometry_s *defaults, char *property, int nw, int nh )
{
	// Assign normalised width and height
	geometry->nw = nw;
	geometry->nh = nh;

	// Assign from defaults if available
	if ( defaults != NULL )
	{
		geometry->x = defaults->x;
		geometry->y = defaults->y;
		geometry->w = geometry->sw = defaults->w;
		geometry->h = geometry->sh = defaults->h;
		geometry->mix = defaults->mix;
		defaults->next = geometry;
	}
	else
	{
		geometry->mix = 100;
	}

	// Parse the geomtry string
	if ( property != NULL && strcmp( property, "" ) )
	{
		char *ptr = property;
		geometry->x = parse_value( &ptr, nw, ',', geometry->x );
		geometry->y = parse_value( &ptr, nh, ':', geometry->y );
		geometry->w = geometry->sw = parse_value( &ptr, nw, 'x', geometry->w );
		geometry->h = geometry->sh = parse_value( &ptr, nh, ':', geometry->h );
		geometry->mix = parse_value( &ptr, 100, ' ', geometry->mix );
	}
}

/** Calculate real geometry.
*/

static void geometry_calculate( struct geometry_s *output, struct geometry_s *in, float position )
{
	// Search in for position
	struct geometry_s *out = in->next;

	if ( position >= 1.0 )
	{
		int section = floor( position );
		position -= section;
		if ( section % 2 == 1 )
			position = 1.0 - position;
	}

	while ( out->next != NULL )
	{
		if ( position >= in->position && position < out->position )
			break;

		in = out;
		out = in->next;
	}

	position = ( position - in->position ) / ( out->position - in->position );

	// Calculate this frames geometry
	if ( in->frame != out->frame - 1 )
	{
		output->nw = in->nw;
		output->nh = in->nh;
		output->x = in->x + ( out->x - in->x ) * position;
		output->y = in->y + ( out->y - in->y ) * position;
		output->w = in->w + ( out->w - in->w ) * position;
		output->h = in->h + ( out->h - in->h ) * position;
		output->mix = in->mix + ( out->mix - in->mix ) * position;
	}
	else
	{
		output->nw = out->nw;
		output->nh = out->nh;
		output->x = out->x;
		output->y = out->y;
		output->w = out->w;
		output->h = out->h;
		output->mix = out->mix;
	}
}

void transition_destroy_keys( void *arg )
{
	struct geometry_s *ptr = arg;
	struct geometry_s *next = NULL;

	while ( ptr != NULL )
	{
		next = ptr->next;
		free( ptr );
		ptr = next;
	}
}

static struct geometry_s *transition_parse_keys( mlt_transition this,  int normalised_width, int normalised_height )
{
	// Loop variable for property interrogation
	int i = 0;

	// Get the properties of the transition
	mlt_properties properties = mlt_transition_properties( this );

	// Get the in and out position
	mlt_position in = mlt_transition_get_in( this );
	mlt_position out = mlt_transition_get_out( this );

	// Create the start
	struct geometry_s *start = calloc( 1, sizeof( struct geometry_s ) );

	// Create the end (we always need two entries)
	struct geometry_s *end = calloc( 1, sizeof( struct geometry_s ) );

	// Pointer
	struct geometry_s *ptr = start;

	// Parse the start property
	geometry_parse( start, NULL, mlt_properties_get( properties, "start" ), normalised_width, normalised_height );

	// Parse the keys in between
	for ( i = 0; i < mlt_properties_count( properties ); i ++ )
	{
		// Get the name of the property
		char *name = mlt_properties_get_name( properties, i );

		// Check that it's valid
		if ( !strncmp( name, "key[", 4 ) )
		{
			// Get the value of the property
			char *value = mlt_properties_get_value( properties, i );

			// Determine the frame number
			int frame = atoi( name + 4 );

			// Determine the position
			float position = 0;
			
			if ( frame >= 0 && frame < ( out - in ) )
				position = ( float )frame / ( float )( out - in + 1 );
			else if ( frame < 0 && - frame < ( out - in ) )
				position = ( float )( out - in + frame ) / ( float )( out - in + 1 );

			// For now, we'll exclude all keys received out of order
			if ( position > ptr->position )
			{
				// Create a new geometry
				struct geometry_s *temp = calloc( 1, sizeof( struct geometry_s ) );

				// Parse and add to the list
				geometry_parse( temp, ptr, value, normalised_width, normalised_height );

				// Assign the position and frame
				temp->frame = frame;
				temp->position = position;

				// Allow the next to be appended after this one
				ptr = temp;
			}
			else
			{
				fprintf( stderr, "Key out of order - skipping %s\n", name );
			}
		}
	}
	
	// Parse the end
	geometry_parse( end, ptr, mlt_properties_get( properties, "end" ), normalised_width, normalised_height );
	if ( out > 0 )
		end->position = ( float )( out - in ) / ( float )( out - in + 1 );
	else
		end->position = 1;

	// Assign to properties to ensure we get destroyed
	mlt_properties_set_data( properties, "geometries", start, 0, transition_destroy_keys, NULL );

	return start;
}

struct geometry_s *composite_calculate( struct geometry_s *result, mlt_transition this, mlt_frame a_frame, float position )
{
	// Get the properties from the transition
	mlt_properties properties = mlt_transition_properties( this );

	// Get the properties from the frame
	mlt_properties a_props = mlt_frame_properties( a_frame );
	
	// Structures for geometry
	struct geometry_s *start = mlt_properties_get_data( properties, "geometries", NULL );

	// Now parse the geometries
	if ( start == NULL )
	{
		// Obtain the normalised width and height from the a_frame
		int normalised_width = mlt_properties_get_int( a_props, "normalised_width" );
		int normalised_height = mlt_properties_get_int( a_props, "normalised_height" );

		// Parse the transitions properties
		start = transition_parse_keys( this, normalised_width, normalised_height );
	}

	// Do the calculation
	geometry_calculate( result, start, position );

	return start;
}

typedef struct 
{
	float matrix[3][3];
}
affine_t;

static void affine_init( float this[3][3] )
{
	this[0][0] = 1;
	this[0][1] = 0;
	this[0][2] = 0;
	this[1][0] = 0;
	this[1][1] = 1;
	this[1][2] = 0;
	this[2][0] = 0;
	this[2][1] = 0;
	this[2][2] = 1;
}

// Multiply two this affine transform with that
static void affine_multiply( float this[3][3], float that[3][3] )
{
	float output[3][3];
	int i;
	int j;

	for ( i = 0; i < 3; i ++ )
		for ( j = 0; j < 3; j ++ )
			output[i][j] = this[i][0] * that[j][0] + this[i][1] * that[j][1] + this[i][2] * that[j][2];

	this[0][0] = output[0][0];
	this[0][1] = output[0][1];
	this[0][2] = output[0][2];
	this[1][0] = output[1][0];
	this[1][1] = output[1][1];
	this[1][2] = output[1][2];
	this[2][0] = output[2][0];
	this[2][1] = output[2][1];
	this[2][2] = output[2][2];
}

// Rotate by a given angle
static void affine_rotate( float this[3][3], float angle )
{
	float affine[3][3];
	affine[0][0] = cos( angle * M_PI / 180 );
	affine[0][1] = 0 - sin( angle * M_PI / 180 );
	affine[0][2] = 0;
	affine[1][0] = sin( angle * M_PI / 180 );
	affine[1][1] = cos( angle * M_PI / 180 );
	affine[1][2] = 0;
	affine[2][0] = 0;
	affine[2][1] = 0;
	affine[2][2] = 1;
	affine_multiply( this, affine );
}

static void affine_rotate_y( float this[3][3], float angle )
{
	float affine[3][3];
	affine[0][0] = cos( angle * M_PI / 180 );
	affine[0][1] = 0;
	affine[0][2] = 0 - sin( angle * M_PI / 180 );
	affine[1][0] = 0;
	affine[1][1] = 1;
	affine[1][2] = 0;
	affine[2][0] = sin( angle * M_PI / 180 );
	affine[2][1] = 0;
	affine[2][2] = cos( angle * M_PI / 180 );
	affine_multiply( this, affine );
}

static void affine_rotate_z( float this[3][3], float angle )
{
	float affine[3][3];
	affine[0][0] = 1;
	affine[0][1] = 0;
	affine[0][2] = 0;
	affine[1][0] = 0;
	affine[1][1] = cos( angle * M_PI / 180 );
	affine[1][2] = sin( angle * M_PI / 180 );
	affine[2][0] = 0;
	affine[2][1] = - sin( angle * M_PI / 180 );
	affine[2][2] = cos( angle * M_PI / 180 );
	affine_multiply( this, affine );
}

static void affine_scale( float this[3][3], float sx, float sy )
{
	float affine[3][3];
	affine[0][0] = sx;
	affine[0][1] = 0;
	affine[0][2] = 0;
	affine[1][0] = 0;
	affine[1][1] = sy;
	affine[1][2] = 0;
	affine[2][0] = 0;
	affine[2][1] = 0;
	affine[2][2] = 1;
	affine_multiply( this, affine );
}

// Shear by a given value
static void affine_shear( float this[3][3], float shear_x, float shear_y, float shear_z )
{
	float affine[3][3];
	affine[0][0] = 1;
	affine[0][1] = tan( shear_x * M_PI / 180 );
	affine[0][2] = 0;
	affine[1][0] = tan( shear_y * M_PI / 180 );
	affine[1][1] = 1;
	affine[1][2] = tan( shear_z * M_PI / 180 );
	affine[2][0] = 0;
	affine[2][1] = 0;
	affine[2][2] = 1;
	affine_multiply( this, affine );
}

static void affine_offset( float this[3][3], int x, int y )
{
	this[0][2] += x;
	this[1][2] += y;
}

// Obtain the mapped x coordinate of the input
static inline double MapX( float this[3][3], int x, int y )
{
	return this[0][0] * x + this[0][1] * y + this[0][2];
}

// Obtain the mapped y coordinate of the input
static inline double MapY( float this[3][3], int x, int y )
{
	return this[1][0] * x + this[1][1] * y + this[1][2];
}

static inline double MapZ( float this[3][3], int x, int y )
{
	return this[2][0] * x + this[2][1] * y + this[2][2];
}

#define MAX( x, y ) x > y ? x : y
#define MIN( x, y ) x < y ? x : y

static void affine_max_output( float this[3][3], float *w, float *h )
{
	int tlx = MapX( this, -720, 576 );
	int tly = MapY( this, -720, 576 );
	int trx = MapX( this, 720, 576 );
	int try = MapY( this, 720, 576 );
	int blx = MapX( this, -720, -576 );
	int bly = MapY( this, -720, -576 );
	int brx = MapX( this, 720, -576 );
	int bry = MapY( this, 720, -576 );

	int max_x;
	int max_y;
	int min_x;
	int min_y;

	max_x = MAX( tlx, trx );
	max_x = MAX( max_x, blx );
	max_x = MAX( max_x, brx );

	min_x = MIN( tlx, trx );
	min_x = MIN( min_x, blx );
	min_x = MIN( min_x, brx );

	max_y = MAX( tly, try );
	max_y = MAX( max_y, bly );
	max_y = MAX( max_y, bry );

	min_y = MIN( tly, try );
	min_y = MIN( min_y, bly );
	min_y = MIN( min_y, bry );

	*w = ( float )( max_x - min_x + 1 ) / 1440.0;
	*h = ( float )( max_y - min_y + 1 ) / 1152.0;
}

#define IN_RANGE( v, r )	( v >= - r / 2 && v < r / 2 )

/** Get the image.
*/

static int transition_get_image( mlt_frame a_frame, uint8_t **image, mlt_image_format *format, int *width, int *height, int writable )
{
	// Get the b frame from the stack
	mlt_frame b_frame = mlt_frame_pop_frame( a_frame );

	// Get the transition object
	mlt_transition this = mlt_frame_pop_service( a_frame );

	// Get the properties of the transition
	mlt_properties properties = mlt_transition_properties( this );

	// Get the properties of the a frame
	mlt_properties a_props = mlt_frame_properties( a_frame );

	// Get the properties of the b frame
	mlt_properties b_props = mlt_frame_properties( b_frame );

	// Image, format, width, height and image for the b frame
	uint8_t *b_image = NULL;
	mlt_image_format b_format = mlt_image_yuv422;
	int b_width;
	int b_height;

	// Get the unique name to retrieve the frame position
	char *name = mlt_properties_get( properties, "_unique_id" );

	// Assign the current position to the name
	mlt_position position =  mlt_properties_get_position( a_props, name );
	mlt_position in = mlt_properties_get_position( properties, "in" );
	mlt_position out = mlt_properties_get_position( properties, "out" );

	// Structures for geometry
	struct geometry_s *start = mlt_properties_get_data( properties, "geometries", NULL );
	struct geometry_s result;

	// Now parse the geometries
	if ( start == NULL )
	{
		// Obtain the normalised width and height from the a_frame
		int normalised_width = mlt_properties_get_int( a_props, "normalised_width" );
		int normalised_height = mlt_properties_get_int( a_props, "normalised_height" );

		// Parse the transitions properties
		start = transition_parse_keys( this, normalised_width, normalised_height );
	}

	// Fetch the a frame image
	mlt_frame_get_image( a_frame, image, format, width, height, 1 );

	// Calculate the region now
	composite_calculate( &result, this, a_frame, ( float )( position ) / ( out - in + 1 ) );

	// Fetch the b frame image
	result.w = ( int )( result.w * *width / result.nw );
	result.h = ( int )( result.h * *height / result.nh );
	result.x = ( int )( result.x * *width / result.nw );
	result.y = ( int )( result.y * *height / result.nh );
	result.w -= ( int )abs( result.w ) % 2;
	result.x -= ( int )abs( result.x ) % 2;
	b_width = result.w;
	b_height = result.h;

	if ( !strcmp( mlt_properties_get( a_props, "rescale.interp" ), "none" ) )
	{
		mlt_properties_set( b_props, "rescale.interp", "nearest" );
		mlt_properties_set_double( b_props, "consumer_aspect_ratio", mlt_properties_get_double( a_props, "aspect_ratio" ) );
	}
	else
	{
		mlt_properties_set( b_props, "rescale.interp", mlt_properties_get( a_props, "rescale.interp" ) );
		mlt_properties_set_double( b_props, "consumer_aspect_ratio", mlt_properties_get_double( a_props, "consumer_aspect_ratio" ) );
	}

	mlt_properties_set( b_props, "distort", mlt_properties_get( properties, "distort" ) );
	mlt_frame_get_image( b_frame, &b_image, &b_format, &b_width, &b_height, 0 );
	result.w = b_width;
	result.h = b_height;

	// Check that both images are of the correct format and process
	if ( *format == mlt_image_yuv422 && b_format == mlt_image_yuv422 )
	{
		register int x, y;
		register int dx, dy;
		double dz;
		float sw, sh;

		// Get values from the transition
		float fix_rotate_x = mlt_properties_get_double( properties, "fix_rotate_x" );
		float fix_rotate_y = mlt_properties_get_double( properties, "fix_rotate_y" );
		float fix_rotate_z = mlt_properties_get_double( properties, "fix_rotate_z" );
		float rotate_x = mlt_properties_get_double( properties, "rotate_x" );
		float rotate_y = mlt_properties_get_double( properties, "rotate_y" );
		float rotate_z = mlt_properties_get_double( properties, "rotate_z" );
		float fix_shear_x = mlt_properties_get_double( properties, "fix_shear_x" );
		float fix_shear_y = mlt_properties_get_double( properties, "fix_shear_y" );
		float fix_shear_z = mlt_properties_get_double( properties, "fix_shear_z" );
		float shear_x = mlt_properties_get_double( properties, "shear_x" );
		float shear_y = mlt_properties_get_double( properties, "shear_y" );
		float shear_z = mlt_properties_get_double( properties, "shear_z" );
		float ox = mlt_properties_get_double( properties, "ox" );
		float oy = mlt_properties_get_double( properties, "oy" );
		int scale = mlt_properties_get_int( properties, "scale" );

		uint8_t *p = *image;
		uint8_t *q = *image;

		int cx = result.x + ( b_width >> 1 );
		int cy = result.y + ( b_height >> 1 );
	
		int lower_x = 0 - cx;
		int upper_x = *width - cx;
		int lower_y = 0 - cy;
		int upper_y = *height - cy;

		int b_stride = b_width << 1;
		int a_stride = *width << 1;
		int x_offset = ( int )result.w >> 1;
		int y_offset = ( int )result.h >> 1;

		uint8_t *alpha = mlt_frame_get_alpha_mask( b_frame );
		uint8_t *mask = mlt_pool_alloc( b_width * b_height );
		uint8_t *pmask = mask;
		float mix;

		affine_t affine;
		affine_init( affine.matrix );
		affine_rotate( affine.matrix, fix_rotate_x + rotate_x * ( position - in ) );
		affine_rotate_y( affine.matrix, fix_rotate_y + rotate_y * ( position - in ) );
		affine_rotate_z( affine.matrix, fix_rotate_z + rotate_z * ( position - in ) );
		affine_shear( affine.matrix, 
					  fix_shear_x + shear_x * ( position - in ), 
					  fix_shear_y + shear_y * ( position - in ),
					  fix_shear_z + shear_z * ( position - in ) );
		affine_offset( affine.matrix, ox, oy );

		if ( scale )
		{
			affine_max_output( affine.matrix, &sw, &sh );
			affine_scale( affine.matrix, sw, sh );
		}
	
		lower_x -= ( lower_x & 1 );
		upper_x -= ( upper_x & 1 );

		q = *image;

		dz = MapZ( affine.matrix, 0, 0 );

		if ( mask != NULL )
			memset( mask, 0, b_width * b_height );

		for ( y = lower_y; y < upper_y; y ++ )
		{
			p = q;

			for ( x = lower_x; x < upper_x; x ++ )
			{
				dx = MapX( affine.matrix, x, y ) / dz + x_offset;
				dy = MapY( affine.matrix, x, y ) / dz + y_offset;

				if ( dx >= 0 && dx < b_width && dy >=0 && dy < b_height )
				{
					if ( alpha == NULL )
					{
						*pmask ++ = 255;
						dx += dx & 1;
						*p ++ = *( b_image + dy * b_stride + ( dx << 1 ) );
						*p ++ = *( b_image + dy * b_stride + ( dx << 1 ) + ( ( x & 1 ) << 1 ) + 1 );
					}
					else
					{
						*pmask ++ = *( alpha + dy * b_width + dx );
						mix = ( float )*( alpha + dy * b_width + dx ) / 255.0;
						dx += dx & 1;
						*p = *p * ( 1 - mix ) + mix * *( b_image + dy * b_stride + ( dx << 1 ) );
						p ++;
						*p = *p * ( 1 - mix ) + mix * *( b_image + dy * b_stride + ( dx << 1 ) + ( ( x & 1 ) << 1 ) + 1 );
						p ++;
					}
				}
				else
				{
					p += 2;
					pmask ++;
				}
			}

			q += a_stride;
		}

		b_frame->get_alpha_mask = NULL;
		mlt_properties_set_data( b_props, "alpha", mask, 0, mlt_pool_release, NULL );
	}

	return 0;
}

/** Affine transition processing.
*/

static mlt_frame transition_process( mlt_transition transition, mlt_frame a_frame, mlt_frame b_frame )
{
	// Get a unique name to store the frame position
	char *name = mlt_properties_get( mlt_transition_properties( transition ), "_unique_id" );

	// Assign the current position to the name
	mlt_properties a_props = mlt_frame_properties( a_frame );
	mlt_properties_set_position( a_props, name, mlt_frame_get_position( a_frame ) );

	// Push the transition on to the frame
	mlt_frame_push_service( a_frame, transition );

	// Push the b_frame on to the stack
	mlt_frame_push_frame( a_frame, b_frame );

	// Push the transition method
	mlt_frame_push_get_image( a_frame, transition_get_image );

	return a_frame;
}

/** Constructor for the filter.
*/

mlt_transition transition_affine_init( char *arg )
{
	mlt_transition transition = mlt_transition_new( );
	if ( transition != NULL )
	{
		mlt_properties_set_int( mlt_transition_properties( transition ), "sx", 1 );
		mlt_properties_set_int( mlt_transition_properties( transition ), "sy", 1 );
		mlt_properties_set( mlt_transition_properties( transition ), "distort", NULL );
		mlt_properties_set( mlt_transition_properties( transition ), "start", "0,0:100%x100%" );
		transition->process = transition_process;
	}
	return transition;
}
