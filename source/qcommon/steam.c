/*
Copyright (C) 2014 Victor Luchits

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "../qcommon/qcommon.h"
#include "../steamlib/steamlib_public.h"

static steamlib_export_t *steamlib_export;
static bool steamlib_initialized = false;

void Steam_LoadLibrary( void );
void Steam_UnloadLibrary( void );

typedef void ( *com_error_t )( int code, const char *format, ... );

/*
* Steam_LoadLibrary
*/
void Steam_LoadLibrary( void )
{
	static steamlib_import_t import;
	void *( *GetSteamLibAPI )(void *);

	import.Com_Error = (com_error_t)Com_Error;
	import.Com_Printf = Com_Printf;
	import.Com_DPrintf = Com_DPrintf;
	import.Cbuf_ExecuteText = Cbuf_ExecuteText;

	Com_Printf( "Loading Steam module... " );

	steamlib_export = GetSteamLibAPI( &import );

	if( !steamlib_export )
	{
		// failure
		Com_Printf( "Failure.\n" );
		Steam_UnloadLibrary();
	}
	else
	{
		Com_Printf( "Success.\n" );
	}
}

/*
* Steam_UnloadLibrary
*/
void Steam_UnloadLibrary( void )
{
	steamlib_export->Shutdown();

	Com_Printf( "Steam module unloaded.\n" );

	steamlib_export = NULL;
	steamlib_initialized = false;
}

/*
* Steam_Init
*/
void Steam_Init( void )
{
	if( steamlib_export ) {
		if( !steamlib_export->Init() ) {
			Com_Printf( "Steam initialization failed.\n" );
			return;
		}
		steamlib_initialized = true;
	}
}

/*
* Steam_RunFrame
*/
void Steam_RunFrame( void )
{
	if( steamlib_initialized ) {
		steamlib_export->RunFrame();
	}
}

/*
* Steam_Shutdown
*/
void Steam_Shutdown( void )
{
	if( steamlib_initialized ) {
		steamlib_export->Shutdown();
	}
}

/*
* Steam_GetSteamID
*/
uint64_t Steam_GetSteamID( void )
{
	if( steamlib_initialized ) {
		return steamlib_export->GetSteamID();
	}
	return 0;
}

/*
* Steam_GetAuthSessionTicket
*/
int Steam_GetAuthSessionTicket( void (*callback)( void *, size_t ) )
{
	if( steamlib_initialized ) {
		return steamlib_export->GetAuthSessionTicket( callback );
	}
	return 0;
}

/*
* Steam_AdvertiseGame
*/
void Steam_AdvertiseGame( const uint8_t *ip, unsigned short port )
{
	if( steamlib_initialized ) {
		steamlib_export->AdvertiseGame( ip, port );
	}
}

/*
* Steam_GetPersonaName
*/
void Steam_GetPersonaName( char *name, size_t namesize )
{
	if( !namesize ) {
		return;
	}

	if( steamlib_initialized ) {
		steamlib_export->GetPersonaName( name, namesize );
	} else {
		name[0] = '\0';
	}
}
