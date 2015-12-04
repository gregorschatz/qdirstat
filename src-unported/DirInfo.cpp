/*
 *   File name:	kdirinfo.cpp
 *   Summary:	Support classes for QDirStat
 *   License:   GPL V2 - See file LICENSE for details.
 *
 *   Author:	Stefan Hundhammer <Stefan.Hundhammer@gmx.de>
 */


#ifdef HAVE_CONFIG_H
#   include <config.h>
#endif

#include <kapp.h>
#include <klocale.h>
#include "kdirinfo.h"
#include "kdirtreeiterators.h"

using namespace QDirStat;



DirInfo::KDirInfo( DirTree *	tree,
		    DirInfo *	parent,
		    bool	asDotEntry )
    : FileInfo( tree, parent )
{
    init();

    if ( asDotEntry )
    {
	_isDotEntry	= true;
	_dotEntry	= 0;
	_name		= ".";
    }
    else
    {
	_isDotEntry	= false;
	_dotEntry	= new DirInfo( tree, this, true );
    }
}


DirInfo::KDirInfo( const QString &	filenameWithoutPath,
		    struct stat	*	statInfo,
		    DirTree    *	tree,
		    DirInfo	*	parent )
    : FileInfo( filenameWithoutPath,
		 statInfo,
		 tree,
		 parent )
{
    init();
    _dotEntry	= new DirInfo( tree, this, true );
}


DirInfo::KDirInfo( const KFileItem	* fileItem,
		    DirTree 		* tree,
		    DirInfo		* parent )
    : FileInfo( fileItem,
		 tree,
		 parent )
{
    init();
    _dotEntry	= new DirInfo( tree, this, true );
}


DirInfo::KDirInfo( DirTree * 		tree,
		    DirInfo * 		parent,
		    const QString &	filenameWithoutPath,
		    mode_t	 	mode,
		    KFileSize	 	size,
		    time_t	 	mtime )
    : FileInfo( tree,
		 parent,
		 filenameWithoutPath,
		 mode,
		 size,
		 mtime )
{
    init();
    _dotEntry	= new DirInfo( tree, this, true );
}


void
DirInfo::init()
{
    _isDotEntry		= false;
    _pendingReadJobs	= 0;
    _dotEntry		= 0;
    _firstChild		= 0;
    _totalSize		= _size;
    _totalBlocks	= _blocks;
    _totalItems		= 0;
    _totalSubDirs	= 0;
    _totalFiles		= 0;
    _latestMtime	= _mtime;
    _isMountPoint	= false;
    _isExcluded		= false;
    _summaryDirty	= false;
    _beingDestroyed	= false;
    _readState		= KDirQueued;
}


DirInfo::~KDirInfo()
{
    _beingDestroyed	= true;
    FileInfo	*child	= _firstChild;


    // Recursively delete all children.

    while ( child )
    {
	FileInfo * nextChild = child->next();
	delete child;
	child = nextChild;
    }


    // Delete the dot entry.

    if ( _dotEntry )
    {
	delete _dotEntry;
    }
}


void
DirInfo::recalc()
{
    // logDebug() << k_funcinfo << this << endl;

    _totalSize		= _size;
    _totalBlocks	= _blocks;
    _totalItems		= 0;
    _totalSubDirs	= 0;
    _totalFiles		= 0;
    _latestMtime	= _mtime;

    FileInfoIterator it( this, KDotEntryAsSubDir );

    while ( *it )
    {
	_totalSize	+= (*it)->totalSize();
	_totalBlocks	+= (*it)->totalBlocks();
	_totalItems	+= (*it)->totalItems() + 1;
	_totalSubDirs	+= (*it)->totalSubDirs();
	_totalFiles	+= (*it)->totalFiles();

	if ( (*it)->isDir() )
	    _totalSubDirs++;

	if ( (*it)->isFile() )
	    _totalFiles++;

	time_t childLatestMtime = (*it)->latestMtime();

	if ( childLatestMtime > _latestMtime )
	    _latestMtime = childLatestMtime;

	++it;
    }

    _summaryDirty = false;
}


void
DirInfo::setMountPoint( bool isMountPoint )
{
    _isMountPoint = isMountPoint;
}


KFileSize
DirInfo::totalSize()
{
    if ( _summaryDirty )
	recalc();

    return _totalSize;
}


KFileSize
DirInfo::totalBlocks()
{
    if ( _summaryDirty )
	recalc();

    return _totalBlocks;
}


int
DirInfo::totalItems()
{
    if ( _summaryDirty )
	recalc();

    return _totalItems;
}


int
DirInfo::totalSubDirs()
{
    if ( _summaryDirty )
	recalc();

    return _totalSubDirs;
}


int
DirInfo::totalFiles()
{
    if ( _summaryDirty )
	recalc();

    return _totalFiles;
}


time_t
DirInfo::latestMtime()
{
    if ( _summaryDirty )
	recalc();

    return _latestMtime;
}


bool
DirInfo::isFinished()
{
    return ! isBusy();
}


void DirInfo::setReadState( KDirReadState newReadState )
{
    // "aborted" has higher priority than "finished"

    if ( _readState == KDirAborted && newReadState == KDirFinished )
	return;

    _readState = newReadState;
}


bool
DirInfo::isBusy()
{
    if ( _pendingReadJobs > 0 && _readState != KDirAborted )
	return true;

    if ( readState() == KDirReading ||
	 readState() == KDirQueued    )
	return true;

    return false;
}


void
DirInfo::insertChild( FileInfo *newChild )
{
    CHECK_PTR( newChild );

    if ( newChild->isDir() || _dotEntry == 0 || _isDotEntry )
    {
	/**
	 * Only directories are stored directly in pure directory nodes -
	 * unless something went terribly wrong, e.g. there is no dot entry to use.
	 * If this is a dot entry, store everything it gets directly within it.
	 *
	 * In any of those cases, insert the new child in the children list.
	 *
	 * We don't bother with this list's order - it's explicitly declared to
	 * be unordered, so be warned! We simply insert this new child at the
	 * list head since this operation can be performed in constant time
	 * without the need for any additional lastChild etc. pointers or -
	 * even worse - seeking the correct place for insertion first. This is
	 * none of our business; the corresponding "view" object for this tree
	 * will take care of such niceties.
	 **/
	newChild->setNext( _firstChild );
	_firstChild = newChild;
	newChild->setParent( this );	// make sure the parent pointer is correct

	childAdded( newChild );		// update summaries
    }
    else
    {
	/*
	 * If the child is not a directory, don't store it directly here - use
	 * this entry's dot entry instead.
	 */
	_dotEntry->insertChild( newChild );
    }
}


void
DirInfo::childAdded( FileInfo *newChild )
{
    if ( ! _summaryDirty )
    {
	_totalSize	+= newChild->size();
	_totalBlocks	+= newChild->blocks();
	_totalItems++;

	if ( newChild->isDir() )
	    _totalSubDirs++;

	if ( newChild->isFile() )
	    _totalFiles++;

	if ( newChild->mtime() > _latestMtime )
	    _latestMtime = newChild->mtime();
    }
    else
    {
	// NOP

	/*
	 * Don't bother updating the summary fields if the summary is dirty
	 * (i.e. outdated) anyway: As soon as anybody wants to know some exact
	 * value a complete recalculation of the entire subtree will be
	 * triggered. On the other hand, if nobody wants to know (which is very
	 * likely) we can save this effort.
	 */
    }

    if ( _parent )
	_parent->childAdded( newChild );
}


void
DirInfo::deletingChild( FileInfo *deletedChild )
{
    /**
     * When children are deleted, things go downhill: Marking the summary
     * fields as dirty (i.e. outdated) is the only thing that can be done here.
     *
     * The accumulated sizes could be updated (by subtracting this deleted
     * child's values from them), but the latest mtime definitely has to be
     * recalculated: The child now being deleted might just be the one with the
     * latest mtime, and figuring out the second-latest cannot easily be
     * done. So we merely mark the summary as dirty and wait until a recalc()
     * will be triggered from outside - which might as well never happen when
     * nobody wants to know some summary field anyway.
     **/

    _summaryDirty = true;

    if ( _parent )
	_parent->deletingChild( deletedChild );

    if ( ! _beingDestroyed && deletedChild->parent() == this )
    {
	/**
	 * Unlink the child from the children's list - but only if this doesn't
	 * happen recursively in the destructor of this object: No use
	 * bothering about the validity of the children's list if this will all
	 * be history anyway in a moment.
	 **/

	unlinkChild( deletedChild );
    }
}


void
DirInfo::unlinkChild( FileInfo *deletedChild )
{
    if ( deletedChild->parent() != this )
    {
	logError() << deletedChild << " is not a child of " << this
		  << " - cannot unlink from children list!" << endl;
	return;
    }

    if ( deletedChild == _firstChild )
    {
	// logDebug() << "Unlinking first child " << deletedChild << endl;
	_firstChild = deletedChild->next();
	return;
    }

    FileInfo *child = firstChild();

    while ( child )
    {
	if ( child->next() == deletedChild )
	{
	    // logDebug() << "Unlinking " << deletedChild << endl;
	    child->setNext( deletedChild->next() );

	    return;
	}

	child = child->next();
    }

    logError() << "Couldn't unlink " << deletedChild << " from "
	      << this << " children list" << endl;
}


void
DirInfo::readJobAdded()
{
    _pendingReadJobs++;

    if ( _parent )
	_parent->readJobAdded();
}


void
DirInfo::readJobFinished()
{
    _pendingReadJobs--;

    if ( _parent )
	_parent->readJobFinished();
}


void
DirInfo::readJobAborted()
{
    _readState = KDirAborted;

    if ( _parent )
	_parent->readJobAborted();
}


void
DirInfo::finalizeLocal()
{
    cleanupDotEntries();
}


void
DirInfo::finalizeAll()
{
    if ( _isDotEntry )
	return;

    FileInfo *child = firstChild();

    while ( child )
    {
	DirInfo * dir = dynamic_cast<KDirInfo *> (child);

	if ( dir && ! dir->isDotEntry() )
	    dir->finalizeAll();

	child = child->next();
    }

    // Optimization: As long as this directory is not finalized yet, it does
    // (very likely) have a dot entry and thus all direct children are
    // subdirectories, not plain files, so we don't need to bother checking
    // plain file children as well - so do finalizeLocal() only after all
    // children are processed. If this step were the first, for directories
    // that don't have any subdirectories finalizeLocal() would immediately
    // get all their plain file children reparented to themselves, so they
    // would need to be processed in the loop, too.

    _tree->sendFinalizeLocal( this ); // Must be sent _before_ finalizeLocal()!
    finalizeLocal();
}


KDirReadState
DirInfo::readState() const
{
    if ( _isDotEntry && _parent )
	return _parent->readState();
    else
	return _readState;
}


void
DirInfo::cleanupDotEntries()
{
    if ( ! _dotEntry || _isDotEntry )
	return;

    // Reparent dot entry children if there are no subdirectories on this level

    if ( ! _firstChild )
    {
	// logDebug() << "Reparenting children of solo dot entry " << this << endl;

	FileInfo *child = _dotEntry->firstChild();
	_firstChild = child;		// Move the entire children chain here.
	_dotEntry->setFirstChild( 0 );	// _dotEntry will be deleted below.

	while ( child )
	{
	    child->setParent( this );
	    child = child->next();
	}
    }


    // Delete dot entries without any children

    if ( ! _dotEntry->firstChild() )
    {
	// logDebug() << "Removing empty dot entry " << this << endl;

	delete _dotEntry;
	_dotEntry = 0;
    }
}



// EOF