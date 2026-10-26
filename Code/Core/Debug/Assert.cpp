/*****************************************************************************
**																			**
**			              Neversoft Entertainment							**
**																		   	**
**				   Copyright (C) 1999 - All Rights Reserved				   	**
**																			**
******************************************************************************
**																			**
**	Project:		Core Library											**
**																			**
**	Module:			Debug (DBG)			 									**
**																			**
**	File name:		assert.cpp												**
**																			**
**	Created by:		05/27/99	-	mjb										**
**																			**
**	Description:	Assert support code										**
**																			**
*****************************************************************************/


/*****************************************************************************
**							  	  Includes									**
*****************************************************************************/

#include <stdio.h>
#include <core/defines.h>
#include <core/support.h>
#include <core/debug.h>

#include <sys/config/config.h>

#ifndef __PLAT_WN32__
#include <gfx/gfxman.h>
#endif // __PLAT_WN32__

#ifdef __PLAT_NGPS__
int DumpUnwindStack( int iMaxDepth, int *pDest );
#include <libdev.h>
#endif

#ifdef __PLAT_NGC__
#include <dolphin.h>
#define _output OSReport
#else
#define _output printf
#endif

#ifdef __PLAT_PS3__
#include <string.h>
#include <ps3_platform.h>
#endif

#ifdef __NOPT_ASSERT__

/*****************************************************************************
**								DBG Information								**
*****************************************************************************/


namespace Dbg
{



/*****************************************************************************
**								  Externals									**
*****************************************************************************/


/*****************************************************************************
**								   Defines									**
*****************************************************************************/

static const int vASSERT_BUFFER_SIZE = 512;

/*****************************************************************************
**								Private Types								**
*****************************************************************************/


/*****************************************************************************
**							  Private Prototypes							**
*****************************************************************************/


/*****************************************************************************
**								 Private Data								**
*****************************************************************************/

static 	AssertTrap*		assert_trap_handler = NULL;
static 	bool			screen_assert_active = false;

// made public for Dbg_ Macros
char*			msg_null_pointer		= "Null Pointer";
char*			msg_misaligned_pointer	= "Pointer not aligned";			 
char*			msg_pointer_to_free_mem	= "Pointer to free mem";
char*			msg_unknown_reason		= "No reason supplied";
char*			msg_type_mismatch		= "Type Mismatch: \"%s\" is of type \"%s\" - not a valid \"%s\"";

/*****************************************************************************
**								 Public Data								**
*****************************************************************************/


/*****************************************************************************
**							   Private Functions							**
*****************************************************************************/

void	set_trap( AssertTrap* trap ) 
{
	assert_trap_handler = trap;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void	screen_assert( bool on ) 
{
	screen_assert_active = on;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

#ifdef __PLAT_PS3__

// THUG PS3 diagnostic build (make -f Makefile.ps3 ASSERTS=1): a failed assert writes
// "[assert] file:line: message" to the log, syncs it to disc and returns, so one run
// reports every assert it reaches instead of hanging on the first. Each assert site is
// logged in full for its first few hits, then only at 10, 100, 1000... hits, so an assert
// that fails every frame neither floods the log nor fsyncs every frame.
namespace
{
	struct AssertSite { const char* file; unsigned line; unsigned hits; };
	enum { kMaxSites = 512, kFullHits = 3, kMaxFullLines = 1000 };
	AssertSite		s_sites[kMaxSites];
	unsigned		s_numSites;
	unsigned		s_fullLines;
	volatile int	s_assertBusy;
	volatile unsigned s_assertsMissed;	// another assert arrived while one was being logged

	const char* short_path( const char* file )
	{
		if ( !file ) return "?";
		const char* p = strstr( file, "/Code/" );
		return p ? p + 6 : file;
	}

	bool log_this_hit( unsigned hits )
	{
		if ( hits <= kFullHits ) return true;
		unsigned n = hits;
		while ( n % 10 == 0 ) n /= 10;
		return n == 1;		// 10, 100, 1000...
	}
}

static void ps3_assert( const char* file, uint line, const char* reason )
{
	// One at a time (any thread); an assert raised while one is being logged (from the
	// logging itself, or another thread) is only counted.
	if ( __sync_lock_test_and_set( &s_assertBusy, 1 ))
	{
		__sync_fetch_and_add( &s_assertsMissed, 1 );
		return;
	}

	AssertSite* site = NULL;
	for ( unsigned i = 0; i < s_numSites; ++i )
	{
		if ( s_sites[i].line == line && s_sites[i].file == file )
		{
			site = &s_sites[i];
			break;
		}
	}
	if ( !site && s_numSites < kMaxSites )
	{
		site = &s_sites[s_numSites++];
		site->file = file;
		site->line = line;
		site->hits = 0;
	}
	unsigned hits = site ? ++site->hits : 1;

	if ( log_this_hit( hits ) && s_fullLines < kMaxFullLines )
	{
		++s_fullLines;
		char text[640];
		const char* why = reason ? reason : "(no message)";
		int n = snprintf( text, sizeof( text ), "[assert] %s:%u: %s", short_path( file ), (unsigned)line, why );
		if ( n < 0 ) n = 0;
		if ( n > (int)sizeof( text ) - 2 ) n = sizeof( text ) - 2;
		while ( n > 0 && ( text[n - 1] == '\n' || text[n - 1] == '\r' )) --n;
		text[n] = 0;
		for ( char* c = text; *c; ++c )
		{
			if ( *c == '\n' || *c == '\r' ) *c = ' ';		// one line per assert
		}
		unsigned missed = __sync_lock_test_and_set( &s_assertsMissed, 0 );
		char hitText[32] = "";
		if ( hits > 1 ) snprintf( hitText, sizeof( hitText ), " (hit %u times)", hits );
		char missedText[48] = "";
		if ( missed ) snprintf( missedText, sizeof( missedText ), " [%u more missed while logging]", missed );
		size_t len = strlen( text );
		snprintf( text + len, sizeof( text ) - len, "%s%s\n", hitText, missedText );
		Ps3Platform::LogNow( text );					// written and synced to disc
		if ( hits == 1 )
		{
			Ps3Platform::LogWhere( "[assert]   at" );	// main loop / script context, first hit only
		}
		if ( s_fullLines == kMaxFullLines )
		{
			Ps3Platform::LogNow( "[assert] too many asserts: no more are logged this run\n" );
		}
	}

	__sync_lock_release( &s_assertBusy );
}

#endif	// __PLAT_PS3__

#ifdef	__NOPT_DEBUG__
void		Assert( char* file, uint line, Signature& sig, char* reason )
#else
void		Assert( char* file, uint line, char* reason )
#endif
{
#ifdef __PLAT_PS3__
	ps3_assert( file, line, reason );		// log and carry on (no trap, no hang)
#else
	static	char		assert_buffer1[vASSERT_BUFFER_SIZE];
#ifdef	__NOPT_DEBUG
	static	char		assert_buffer2[vASSERT_BUFFER_SIZE];
#endif	
	static	char		assert_buffer3[vASSERT_BUFFER_SIZE];
	static	char*		tmp1 = assert_buffer1; 
#ifdef	__NOPT_DEBUG
	static	char*		tmp2 = assert_buffer2; 
#endif	
	static	char*		tmp3 = assert_buffer3; 


#if !( defined ( __PLAT_XBOX__ ) || defined ( __PLAT_WN32__ ))
	
	static int again = 0;
	if (again) 
	{
		_output ("MEM CONTEXT: %s\n",Mem::Manager::sHandle().GetContextName());
				
		_output( "LOOPED ASSERTION: %s(%d)\n%s\n\n", 
			file, line, reason );
		_output ("Real Assertion: %s\n%s\n",tmp1,tmp3);
		while (1);			// and hang... 
	}
	again = 1;
	
	_output ("\n--------------------------------------------------\nPLEASE COPY FROM A FEW LINES ABOVE HERE\n\nCURRENT MEM CONTEXT: %s\n\n"
			,Mem::Manager::sHandle().GetContextName());
	Mem::Manager& mem_man = Mem::Manager::sHandle();
//	Mem::Heap* heap = mem_man.TopDownHeap();
//	Mem::Region* region = heap->ParentRegion();
//	_output ("TopDown  Fragmentation %7dK, in %5d Blocks\n",heap->mFreeMem.m_count / 1024, heap->mFreeBlocks.m_count);
//	_output ("         used          %7dK, in %5d Blocks\n",heap->mUsedMem.m_count / 1024, heap->mUsedBlocks.m_count);
//	heap = mem_man.BottomUpHeap();
//	_output ("BottomUp Fragmentation %7dK, in %5d Blocks\n",heap->mFreeMem.m_count / 1024, heap->mFreeBlocks.m_count);
//	_output ("         used          %7dK, in %5d Blocks\n",heap->mUsedMem.m_count / 1024, heap->mUsedBlocks.m_count);
//	_output ("Shared Region %dK free out of %d K available\n", region->MemAvailable() / 1024, region->TotalSize() / 1024 );

	_output("Name            Used  Frag  Free   Min  Blocks\n");
	_output("--------------- ----- ----- ---- ------ ------\n");
	Mem::Heap* heap;
	for (heap = mem_man.FirstHeap(); heap != NULL; heap = mem_man.NextHeap(heap))
	{		
			Mem::Region* region = heap->ParentRegion();			
			_output( "%12s: %5dK %4dK %4dK %4dK  %5d \n",
					heap->GetName(),
					heap->mUsedMem.m_count / 1024,
					heap->mFreeMem.m_count / 1024,
					region->MemAvailable() / 1024,
					region->MinMemAvailable() / 1024,
					heap->mUsedBlocks.m_count
					);										
	}
	
	_output( "FILE:      %s(%d)\nASSERTION: %s\n\n", 
		file, line, reason ); 
	_output( tmp1, "FILE:      %s(%d) ", file, line ); 
	_output( tmp3, "ASSERTION: %s", reason ); 
	// attempt to dump the call stack
	// requires that you have the correct .map file, along with the executable
	#ifndef __PLAT_WN32__
	MemView_FindLeaks();
	#endif
		
	_output( "\nCALL STACK ..........................\n\n");
	
	#ifdef __PLAT_NGPS__
	DumpUnwindStack( 40, NULL );
	#endif
		
	
#endif

	sprintf( tmp1, "FILE:      %s(%d) ", file, line ); 
	sprintf( tmp3, "ASSERTION: %s", reason ); 

#ifndef __PLAT_NGC__
#ifndef __PLAT_WN32__
// Mick: Attempt to save a screenshot
//	Dbg_Printf("Attempting to save screenshot 'screens\\Assert???.bmp'\n");
	if (!Config::CD())
	{
		Gfx::Manager * gfx_manager = Gfx::Manager::Instance();
		gfx_manager->ScreenShot( "Assert" );
	}
#endif

	if ( screen_assert_active )
	{
		screen_assert_active = false;

#ifndef __PLAT_WN32__
		Gfx::Manager* gfx_man = Gfx::Manager::Instance();

		gfx_man->AssertText ( 0, tmp1 );

#ifdef	__NOPT_DEBUG
		sprintf( tmp2, "%s", &sig.GetName() ); 
		gfx_man->AssertText ( 1, tmp2 );
#endif
		gfx_man->AssertText ( 2, tmp3 );

		gfx_man->AssertFlush();
#endif	// __PLAT_WN32__
	}
#endif		// __PLAT_NGC__

#ifdef	__NOPT_DEBUG
	sprintf( tmp1, "ASSERTION: %s(%d) %s\n%s\n\n", 
		file, line, &sig.GetName(), reason ); 
#else
	sprintf( tmp1, "FILE:      %s(%d)\nASSERTION: %s\n\n", 
		file, line, reason ); 
#endif


#ifdef	__PLAT_NGPS__
#if 0 	// does not seem to output anything...
	sceDevConsInit(); // Initialise screen console output
	int console = sceDevConsOpen(
	(2048 - 640/2 + 20) << 4, // top left GS X primitive coord
	(2048 - 448/2 + 20) << 4, // top left GS Y primitive coord
	80, // Number of chars (X)
	5); // Number of chars (Y)
	sceDevConsAttribute(console, 7); // Output in white (default colours can be
									// redefined by sceDevConsSetColor)
	sceDevConsClear(console);
	sceDevConsPrintf(console,	tmp1);
	sceDevConsPrintf(console,	"blah \n");
	sceDevConsPrintf(console,	"blah2 \n");
	sceDevConsPrintf(console,	"blah3 \n");
	sceDevConsDraw(console);	
#endif
#endif


	if ( assert_trap_handler != NULL )
	{
		_output ("MEM CONTEXT: %s\n",Mem::Manager::sHandle().GetContextName());

		Dbg_Printf( "%s\n", tmp1 );
#ifdef __PLAT_WN32__
		__debugbreak();
#endif
		assert_trap_handler( tmp1 );
	}
	else
	{						   
		Dbg_Printf( "%s\n", tmp1 );
		Dbg_Printf( "!!NO TRAP HANDLER SET!!\n" );
	}
#endif	// __PLAT_PS3__
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

#if ( defined ( __PLAT_XBOX__ ) || defined ( __PLAT_WN32__ ))
void		assert_vcc( char* file, uint line, char* reason )
{
	static	char		assert_buffer[vASSERT_BUFFER_SIZE];
	static	char*		tmp = assert_buffer; 

	sprintf( tmp, "ASSERTION: %s(%d)\n%s\n\n", 
		file, line, reason ); 

	if ( assert_trap_handler != NULL )
	{
		Dbg_Printf( "%s\n", tmp );
		assert_trap_handler( tmp );
	}
	else
	{
		Dbg_Printf( "%s\n", tmp );
		Dbg_Printf( "!!NO TRAP HANDLER SET!!\n" );
	}
}
#endif // #ifdef __PLAT_XBOX__

/*****************************************************************************
**							   Public Functions								**
*****************************************************************************/

} // namespace Dbg

#endif	//__NOPT_DEBUG__
