/*
vgl_shim.c - vitaGL custom immediate mode shim
Copyright (C) 2023 fgsfds

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <vitaGL.h>

#include "port.h"
#include "xash3d_types.h"
#include "cvardef.h"
#include "const.h"
#include "com_model.h"
#include "cl_entity.h"
#include "render_api.h"
#include "protocol.h"
#include "dlight.h"
#include "ref_api.h"
#include "com_strings.h"
#include "vgl_shim.h"

#define MAX_SHADERLEN 4096
#define MAX_PROGS 32

extern ref_api_t gEngfuncs;

enum vgl_attrib_e
{
	VGL_ATTR_POS       = 0, // 1
	VGL_ATTR_COLOR     = 1, // 2
	VGL_ATTR_TEXCOORD0 = 2, // 4
	VGL_ATTR_TEXCOORD1 = 3, // 8
	VGL_ATTR_NORMAL    = 4, // 16
	VGL_ATTR_MAX
};

// continuation of previous enum
enum vgl_flag_e
{
	VGL_FLAG_ALPHA_TEST = VGL_ATTR_MAX,
	VGL_FLAG_MAX
};

typedef struct
{
	GLuint flags;
	GLint attridx[VGL_ATTR_MAX];
	GLuint glprog;
	GLint ucolor;
	GLint ualpha;
	GLint utex0;
	GLint utex1;
} vgl_prog_t;

static const char *vgl_vert_src =
#include "vgl_shaders/vertex.cg.inc"
;

static const char *vgl_frag_src =
#include "vgl_shaders/fragment.cg.inc"
;

static int vgl_init = 0;

static struct
{
	GLfloat *attrbuf[VGL_ATTR_MAX];
	GLuint cur_flags;
	GLint begin;
	GLint end;
	GLenum prim;
	GLfloat color[4];
	GLfloat alpharef;
	vgl_prog_t progs[MAX_PROGS];
	vgl_prog_t *cur_prog;
	GLboolean uchanged;
} vgl;

static const int vgl_attr_size[VGL_ATTR_MAX] = { 3, 4, 2, 2, 3 };

static const char *vgl_flag_name[VGL_FLAG_MAX] =
{
	"ATTR_POSITION",
	"ATTR_COLOR",
	"ATTR_TEXCOORD0",
	"ATTR_TEXCOORD1",
	"ATTR_NORMAL",
	"FEAT_ALPHA_TEST",
};

static const char *vgl_attr_name[VGL_ATTR_MAX] =
{
	"inPosition",
	"inColor",
	"inTexCoord0",
	"inTexCoord1",
	"inNormal",
};

// HACK: borrow alpha test flag from internal vitaGL state
extern GLboolean alpha_test_state;

static GLuint VGL_GenerateShader( const vgl_prog_t *prog, GLenum type )
{
	char *shader, shader_buf[MAX_SHADERLEN + 1];
	char tmp[256];
	int i;
	GLint status, len;
	GLuint id;

	shader = shader_buf;
	shader[0] = '\n';
	shader[1] = 0;

	for ( i = 0; i < VGL_FLAG_MAX; ++i )
	{
		snprintf( tmp, sizeof( tmp ), "#define %s %d\n", vgl_flag_name[i], prog->flags & ( 1 << i ) );
		strncat( shader, tmp, MAX_SHADERLEN );
	}

	if ( type == GL_FRAGMENT_SHADER )
		strncat( shader, vgl_frag_src, MAX_SHADERLEN );
	else
		strncat( shader, vgl_vert_src, MAX_SHADERLEN );

	id = glCreateShader( type );
	len = strlen( shader );
	glShaderSource( id, 1, (const void *)&shader, &len );
	glCompileShader( id );
	glGetShaderiv( id, GL_COMPILE_STATUS, &status );
	if ( status == GL_FALSE )
	{
		gEngfuncs.Con_Reportf( S_ERROR "VGL_GenerateShader( 0x%04x, 0x%x ): compile failed:\n", prog->flags, type );
		gEngfuncs.Con_Printf( "Shader text:\n%s\n\n", shader );
		glDeleteShader( id );
		return 0;
	}

	return id;
}

static vgl_prog_t *VGL_GetProg( const GLuint flags )
{
	int i, loc, status;
	GLuint vp, fp, glprog;
	vgl_prog_t *prog;

	// try to find existing prog matching this feature set

	if ( vgl.cur_prog && vgl.cur_prog->flags == flags )
		return vgl.cur_prog;

	for ( i = 0; i < MAX_PROGS; ++i )
	{
		if ( vgl.progs[i].flags == flags )
			return &vgl.progs[i];
		else if ( vgl.progs[i].flags == 0 )
			break;
	}

	if ( i == MAX_PROGS )
	{
		gEngfuncs.Host_Error( "VGL_GetProg(): Ran out of program slots for 0x%04x\n", flags );
		return NULL;
	}

	// new prog; generate shaders

	gEngfuncs.Con_DPrintf( S_NOTE "VGL_GetProg(): Generating progs for 0x%04x\n", flags );
	prog = &vgl.progs[i];
	prog->flags = flags;

	vp = VGL_GenerateShader( prog, GL_VERTEX_SHADER );
	fp = VGL_GenerateShader( prog, GL_FRAGMENT_SHADER );
	if ( !vp || !fp )
	{
		prog->flags = 0;
		return NULL;
	}

	glprog = glCreateProgram();
	glAttachShader( glprog, vp );
	glAttachShader( glprog, fp );

	loc = 0;
	for ( i = 0; i < VGL_ATTR_MAX; ++i )
	{
		if ( flags & ( 1 << i ) )
		{
			prog->attridx[i] = loc;
			glBindAttribLocation( glprog, loc++, vgl_attr_name[i] );
		}
		else
		{
			prog->attridx[i] = -1;
		}
	}

	glLinkProgram( glprog );
	glDeleteShader( vp );
	glDeleteShader( fp );

	glGetProgramiv( glprog, GL_LINK_STATUS, &status );
	if ( status == GL_FALSE )
	{
		gEngfuncs.Con_Reportf( S_ERROR "VGL_GetProg( 0x%04x ): link failed!\n", prog->flags );
		prog->flags = 0;
		glDeleteProgram( glprog );
		return NULL;
	}

	prog->ucolor = glGetUniformLocation( glprog, "uColor" );
	prog->ualpha = glGetUniformLocation( glprog, "uAlphaTest" );
	prog->utex0  = glGetUniformLocation( glprog, "uTex0" );
	prog->utex1  = glGetUniformLocation( glprog, "uTex1" );

	// these never change
	if ( prog->utex0 >= 0 )
		glUniform1i( prog->utex0, 0 );
	if ( prog->utex1 >= 0 )
		glUniform1i( prog->utex1, 1 );

	prog->glprog = glprog;

	gEngfuncs.Con_DPrintf( S_NOTE "VGL_GetProg(): Generated progs for 0x%04x: glprog=%u ucolor=%d ualpha=%d\n", flags, glprog, prog->ucolor, prog->ualpha );

	return prog;
}

static vgl_prog_t *VGL_SetProg( const GLuint flags )
{
	vgl_prog_t *prog = NULL;

	if ( flags && ( prog = VGL_GetProg( flags ) ) )
	{
		if ( prog != vgl.cur_prog )
		{
			glUseProgram( prog->glprog );
			vgl.uchanged = GL_TRUE;
		}
		if ( vgl.uchanged )
		{
			if ( prog->ualpha >= 0 )
				glUniform1f( prog->ualpha, vgl.alpharef );
			if ( prog->ucolor >= 0 )
				glUniform4fv( prog->ucolor, 1, vgl.color );
			vgl.uchanged = GL_FALSE;
		}
	}
	else
	{
		glUseProgram( 0 );
	}

	vgl.cur_prog = prog;
	return prog;
}

int VGL_ShimInit( void )
{
	int i;
	GLuint total, size;

	if ( vgl_init )
		return 0;

	memset( &vgl, 0, sizeof( vgl ) );

	vgl.color[0] = 1.f;
	vgl.color[1] = 1.f;
	vgl.color[2] = 1.f;
	vgl.color[3] = 1.f;
	vgl.uchanged = GL_TRUE;

	total = 0;
	for ( i = 0; i < VGL_ATTR_MAX; ++i )
	{
		size = VGL_MAX_VERTS * vgl_attr_size[i] * sizeof( GLfloat );
		vgl.attrbuf[i] = memalign( 0x100, size );
		total += size;
	}

	VGL_ShimInstall();

	gEngfuncs.Con_DPrintf( S_NOTE "VGL_ShimInit(): %u bytes allocated for vertex buffer\n", total );

	vgl_init = 1;
	return 0;
}

void VGL_ShimShutdown( void )
{
	int i;

	if ( !vgl_init )
		return;

	glFinish();
	glUseProgram( 0 );

	for ( i = 0; i < MAX_PROGS; ++i )
	{
		if ( vgl.progs[i].flags )
			glDeleteProgram( vgl.progs[i].glprog );
	}

	for ( i = 0; i < VGL_ATTR_MAX; ++i )
		free( vgl.attrbuf[i] );

	memset( &vgl, 0, sizeof( vgl ) );

	vgl_init = 0;
}

void VGL_ShimEndFrame( void )
{
	vgl.end = vgl.begin = 0;
}

void VGL_Begin( GLenum prim )
{
	int i;
	vgl.prim = prim;
	vgl.begin = vgl.end;
	// pos always enabled
	vgl.cur_flags = 1 << VGL_ATTR_POS;
	// disable all vertex attrib pointers
	for ( i = 0; i < VGL_ATTR_MAX; ++i )
		glDisableVertexAttribArray( i );
}

void VGL_End( void )
{
	int i;
	vgl_prog_t *prog;
	GLuint flags = vgl.cur_flags;
	GLint count = vgl.end - vgl.begin;

	if ( !vgl.prim || !count )
		goto _leave; // end without begin 

	if ( vgl.end > VGL_MAX_VERTS )
	{
		gEngfuncs.Con_Reportf( S_ERROR "VGL_End(): Ran out of vertex space (%d overhead)\n", vgl.end - VGL_MAX_VERTS );
		vgl.end = 0;
		goto _leave;
	}

	// enable alpha test if needed
	if ( alpha_test_state )
		flags |= 1 << VGL_FLAG_ALPHA_TEST;

	prog = VGL_SetProg( flags );
	if ( !prog )
	{
		gEngfuncs.Host_Error( "VGL_End(): Could not find program for flags 0x%04x!\n", flags );
		goto _leave;
	}

	for ( i = 0; i < VGL_ATTR_MAX; ++i )
	{
		if ( prog->attridx[i] >= 0 )
		{
			glEnableVertexAttribArray( prog->attridx[i] );
			glVertexAttribPointer( prog->attridx[i], vgl_attr_size[i], GL_FLOAT, GL_FALSE, 0, vgl.attrbuf[i] + vgl_attr_size[i] * vgl.begin );
		}
	}

	glDrawArrays( vgl.prim, 0, count );

_leave:
	vgl.prim = GL_NONE;
	vgl.begin = vgl.end;
	vgl.cur_flags = 0;
}

void VGL_Vertex3f( GLfloat x, GLfloat y, GLfloat z )
{
	GLfloat *p = vgl.attrbuf[VGL_ATTR_POS] + vgl.end * 3;
	*p++ = x;
	*p++ = y;
	*p++ = z;
	++vgl.end;
	if ( vgl.end >= VGL_MAX_VERTS )
	{
		gEngfuncs.Con_Reportf( S_ERROR "VGL_Vertex3f(): Vertex buffer overflow!\n" );
		vgl.end = vgl.begin = 0;
	}
}

void VGL_Vertex2f( GLfloat x, GLfloat y )
{
	VGL_Vertex3f( x, y, 0.f );
}

void VGL_Vertex3fv( const GLfloat *v )
{
	VGL_Vertex3f( v[0], v[1], v[2] );
}

void VGL_Color4f( GLfloat r, GLfloat g, GLfloat b, GLfloat a )
{
	vgl.color[0] = r;
	vgl.color[1] = g;
	vgl.color[2] = b;
	vgl.color[3] = a;
	vgl.uchanged = GL_TRUE;
	if ( vgl.prim )
	{
		// HACK: enable color attribute if we're using color inside a Begin-End pair
		GLfloat *p = vgl.attrbuf[VGL_ATTR_COLOR] + vgl.end * 4;
		vgl.cur_flags |= 1 << VGL_ATTR_COLOR;
		*p++ = r;
		*p++ = g;
		*p++ = b;
		*p++ = a;
	}
}

void VGL_Color3f( GLfloat r, GLfloat g, GLfloat b )
{
	VGL_Color4f( r, g, b, 1.f );
}

void VGL_Color4ub( GLubyte r, GLubyte g, GLubyte b, GLubyte a )
{
	VGL_Color4f( (GLfloat)r / 255.f, (GLfloat)g / 255.f, (GLfloat)b / 255.f, (GLfloat)a / 255.f );
}

void VGL_Color4ubv( const GLubyte *v )
{
	VGL_Color4ub( v[0], v[1], v[2], v[3] );
}

void VGL_TexCoord2f( GLfloat u, GLfloat v )
{
	// by spec glTexCoord always updates texunit 0
	GLfloat *p = vgl.attrbuf[VGL_ATTR_TEXCOORD0] + vgl.end * 2;
	vgl.cur_flags |= 1 << VGL_ATTR_TEXCOORD0;
	*p++ = u;
	*p++ = v;
}

void VGL_MultiTexCoord2f( GLenum tex, GLfloat u, GLfloat v )
{
	GLfloat *p;
	// assume there can only be two
	if ( tex == GL_TEXTURE0 )
	{
		p = vgl.attrbuf[VGL_ATTR_TEXCOORD0] + vgl.end * 2;
		vgl.cur_flags |= 1 << VGL_ATTR_TEXCOORD0;
	}
	else
	{
		p = vgl.attrbuf[VGL_ATTR_TEXCOORD1] + vgl.end * 2;
		vgl.cur_flags |= 1 << VGL_ATTR_TEXCOORD1;
	}
	*p++ = u;
	*p++ = v;
}

void VGL_Normal3fv( const GLfloat *v )
{
	/* not sure if we actually need these */
}

void VGL_ShadeModel( GLenum unused )
{
	/* this doesn't do anything in vitaGL except spit errors in debug mode, so stub it out */
}

void VGL_AlphaFunc( GLenum mode, GLfloat ref )
{
	vgl.alpharef = ref;
	vgl.uchanged = GL_TRUE;
	// mode is always GL_GREATER
}

void VGL_DrawBuffer( GLenum mode )
{
	/* unsupported */
}

#define VGL_DECLARE_PTR( name ) extern void *pgl ## name
#define VGL_OVERRIDE_PTR( name ) pgl ## name = VGL_ ## name

void VGL_ShimInstall( void )
{
	VGL_DECLARE_PTR( Vertex2f );
	VGL_DECLARE_PTR( Vertex3f );
	VGL_DECLARE_PTR( Vertex3fv );
	VGL_DECLARE_PTR( Color3f );
	VGL_DECLARE_PTR( Color4f );
	VGL_DECLARE_PTR( Color4ub );
	VGL_DECLARE_PTR( Color4ubv );
	VGL_DECLARE_PTR( Normal3fv );
	VGL_DECLARE_PTR( TexCoord2f );
	VGL_DECLARE_PTR( MultiTexCoord2f );
	VGL_DECLARE_PTR( ShadeModel );
	VGL_DECLARE_PTR( DrawBuffer );
	VGL_DECLARE_PTR( AlphaFunc );
	VGL_DECLARE_PTR( Begin );
	VGL_DECLARE_PTR( End );
	VGL_OVERRIDE_PTR( Vertex2f );
	VGL_OVERRIDE_PTR( Vertex3f );
	VGL_OVERRIDE_PTR( Vertex3fv );
	VGL_OVERRIDE_PTR( Color3f );
	VGL_OVERRIDE_PTR( Color4f );
	VGL_OVERRIDE_PTR( Color4ub );
	VGL_OVERRIDE_PTR( Color4ubv );
	VGL_OVERRIDE_PTR( Normal3fv );
	VGL_OVERRIDE_PTR( TexCoord2f );
	VGL_OVERRIDE_PTR( MultiTexCoord2f );
	VGL_OVERRIDE_PTR( ShadeModel );
	VGL_OVERRIDE_PTR( DrawBuffer );
	VGL_OVERRIDE_PTR( AlphaFunc );
	VGL_OVERRIDE_PTR( Begin );
	VGL_OVERRIDE_PTR( End );
}