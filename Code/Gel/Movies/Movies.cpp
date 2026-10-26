/*****************************************************************************
**																			**
**			              Neversoft Entertainment			                **
**																		   	**
**				   Copyright (C) 1999 - All Rights Reserved				   	**
**																			**
******************************************************************************
**																			**
**	Project:		GEL (Game Engine Library)								**
**																			**
**	Module:			GEL					 									**
**																			**
**	File name:		movies.cpp												**
**																			**
**	Created:		5/14/1	-	mjd											**
**																			**
**	Description:	streaming movies										**
**																			**
*****************************************************************************/


/*****************************************************************************
**							  	  Includes									**
*****************************************************************************/

#include <core/defines.h>
#include <core/macros.h>
#include <core/singleton.h>

#ifdef __PLAT_NGPS__
#include <gel/movies/ngps/p_movies.h>
#elif defined( __PLAT_XBOX__ )
#include <gel/movies/xbox/p_movies.h>
#elif defined( __PLAT_NGC__ )
#include <gel/movies/ngc/p_movies.h>
#endif

#include <sys/profiler.h>

#ifdef __PLAT_PS3__
namespace Ps3Movie { void Play( const char *pName ); }	// Code/Ps3/video/ps3_movie.cpp
#endif


/*****************************************************************************
**								DBG Information								**
*****************************************************************************/



namespace Flx
{



void PlayMovie( const char *pMovieName )
{
#ifdef __PLAT_PS3__
	Ps3Movie::Play( pMovieName );
#endif
	return; // lwss: stubbed for now..
	//PMovies_PlayMovie( pMovieName );
}

}  // namespace Flx
