#include "plfs.h"
#include "plfs_private.h"
#include "COPYRIGHT.h"
#include "IOStore.h"
#include "WriteFile.h"
#include "Container.h"

#include <assert.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/dir.h>
#include <string>
using namespace std;

// the path here is a physical path.  A WriteFile just uses one hostdir.
// so the path sent to WriteFile should be the physical path to the
// shadow or canonical container (i.e. not relying on symlinks)
// anyway, this should all happen above WriteFile and be transparent to
// WriteFile.  This comment is for educational purposes only.
WriteFile::WriteFile(string path, string newhostname,
                     mode_t newmode, size_t buffer_mbs,
                     struct plfs_backend *backend) : Metadata::Metadata()
{
    this->container_path    = path;
    this->subdir_path       = path;     /* not really a subdir */
    this->subdirback        = backend;
    this->hostname          = newhostname;
    this->index             = NULL;
    this->mode              = newmode;
    this->has_been_renamed  = false;
    this->createtime        = Util::getTime();
    this->write_count       = 0;
    this->index_buffer_mbs  = buffer_mbs;
    this->max_writers       = 0;
    pthread_mutex_init( &data_mux, NULL );
    pthread_mutex_init( &index_mux, NULL );
}

void WriteFile::setContainerPath ( string p )
{
    this->container_path    = p;
    this->has_been_renamed = true;
}

/**
 * WriteFile::setSubdirPath: change the subdir path and backend
 *
 * XXXCDC: I think this might need some mutex protection, and we
 * should only allow it if the current number of writers is zero,
 * since all writers on one host currently must use the same
 * hostdir/backend.
 *
 * @param p new subdir (either on shadow or canonical)
 * @param wrback backend to use to access the subdir
 */
void WriteFile::setSubdirPath (string p, struct plfs_backend *wrback)
{
    this->subdir_path     = p;
    this->subdirback      = wrback;
}

WriteFile::~WriteFile()
{
    mlog(WF_DAPI, "Delete self %s", container_path.c_str() );
    Close();
    if ( index ) {
        closeIndex();
        delete index;
        index = NULL;
    }
    pthread_mutex_destroy( &data_mux );
    pthread_mutex_destroy( &index_mux );
}


/* ret 0 or -err */
int WriteFile::sync()
{
    int ret = 0;
    // iterate through and sync all open fds
    Util::MutexLock( &data_mux, __FUNCTION__ );
    map<pid_t, OpenFh >::iterator pids_itr;
    for( pids_itr = fhs.begin(); pids_itr != fhs.end() && ret==0; pids_itr++ ) {
        ret = pids_itr->second.fh->Fsync();
    }
    Util::MutexUnlock( &data_mux, __FUNCTION__ );

    // now sync the index
    Util::MutexLock( &index_mux, __FUNCTION__ );
    if ( ret == 0 ) {
        index->flush();
    }
    if ( ret == 0 ) {
        IOSHandle *syncfh;
        struct plfs_backend *ib;
        syncfh = index->getFh(&ib);
        //XXXCDC:iostore maybe index->flush() should have a sync param?
        ret = syncfh->Fsync();
    }
    Util::MutexUnlock( &index_mux, __FUNCTION__ );
    return ret;
}

/* ret 0 or -err */
int WriteFile::sync( pid_t pid )
{
    int ret=0;
    OpenFh *ofh = getFh( pid );
    if ( ofh == NULL ) {
        // ugh, sometimes FUSE passes in weird pids, just ignore this
        //ret = -ENOENT;
    } else {
        ret = ofh->fh->Fsync();
        Util::MutexLock( &index_mux, __FUNCTION__ );
        if ( ret == 0 ) {
            index->flush();
        }
        if ( ret == 0 ) {
            IOSHandle *syncfh;
            struct plfs_backend *ib;
            syncfh = index->getFh(&ib);
            //XXXCDC:iostore maybe index->flush() should have a sync param?
            ret  = syncfh->Fsync();
        }
        Util::MutexUnlock( &index_mux, __FUNCTION__ );
    }
    return ret;
}


// returns -err or number of writers
int WriteFile::addWriter( pid_t pid, bool child )
{
    int ret = 0;
    Util::MutexLock(   &data_mux, __FUNCTION__ );
    struct OpenFh *ofh = getFh( pid );
    if (ofh != NULL) {
        ofh->writers++;
    } else {
        /* note: this uses subdirback from object to open */
        IOSHandle *fh;
        fh = openDataFile( subdir_path, hostname, pid, DROPPING_MODE, ret);
        if ( fh != NULL ) {
            struct OpenFh xofh;
            xofh.writers = 1;
            xofh.fh = fh;
            fhs[pid] = xofh;
        } // else, ret was already set
    }
    int writers = incrementOpens(0);
    if ( ret == 0 && ! child ) {
        writers = incrementOpens(1);
    }
    max_writers++;
    mlog(WF_DAPI, "%s (%d) on %s now has %d writers",
         __FUNCTION__, pid, container_path.c_str(), writers );
    Util::MutexUnlock( &data_mux, __FUNCTION__ );
    return ( ret == 0 ? writers : ret );
}

size_t WriteFile::numWriters( )
{
    int writers = incrementOpens(0);
    bool paranoid_about_reference_counting = false;
    if ( paranoid_about_reference_counting ) {
        int check = 0;
        Util::MutexLock(   &data_mux, __FUNCTION__ );
        map<pid_t, OpenFh >::iterator pids_itr;
        for( pids_itr = fhs.begin(); pids_itr != fhs.end(); pids_itr++ ) {
            check += pids_itr->second.writers;
        }
        if ( writers != check ) {
            mlog(WF_DRARE, "%s %d not equal %d", __FUNCTION__,
                 writers, check );
            assert( writers==check );
        }
        Util::MutexUnlock( &data_mux, __FUNCTION__ );
    }
    return writers;
}

// ok, something is broken again.
// it shows up when a makefile does a command like ./foo > bar
// the bar gets 1 open, 2 writers, 1 flush, 1 release
// and then the reference counting is screwed up
// the problem is that a child is using the parents fd
struct OpenFh *WriteFile::getFh( pid_t pid ) {
    map<pid_t,OpenFh>::iterator itr;
    struct OpenFh *ofh = NULL;
    if ( (itr = fhs.find( pid )) != fhs.end() ) {
        /*
            ostringstream oss;
            oss << __FILE__ << ":" << __FUNCTION__ << " found fd "
                << itr->second->fd << " with writers "
                << itr->second->writers
                << " from pid " << pid;
            mlog(WF_DCOMMON, "%s", oss.str().c_str() );
        */
        ofh = &(itr->second);
    } else {
        // here's the code that used to do it so a child could share
        // a parent fd but for some reason I commented it out
        /*
           // I think this code is a mistake.  We were doing it once
           // when a child was writing to a file that the parent opened
           // but shouldn't it be OK to just give the child a new datafile?
        if ( fds.size() > 0 ) {
            ostringstream oss;
            // ideally instead of just taking a random pid, we'd rather
            // try to find the parent pid and look for it
            // we need this code because we've seen in FUSE that an open
            // is done by a parent but then a write comes through as the child
            mlog(WF_DRARE, "%s WARNING pid %d is not mapped. "
                    "Borrowing fd %d from pid %d",
                    __FILE__, (int)pid, (int)fds.begin()->second->fd,
                    (int)fds.begin()->first );
            ofd = fds.begin()->second;
        } else {
            mlog(WF_DCOMMON, "%s no fd to give to %d", __FILE__, (int)pid);
            ofd = NULL;
        }
        */
        mlog(WF_DCOMMON, "%s no fh to give to %d", __FILE__, (int)pid);
        ofh = NULL;
    }
    return ofh;
}

/* ret 0 or -err */
/* uses this->subdirback for close */
int WriteFile::closeFh(IOSHandle *fh)
{
    map<IOSHandle *,string>::iterator paths_itr;
    paths_itr = paths.find( fh );
    string path = ( paths_itr == paths.end() ? "ENOENT?" : paths_itr->second );
    int ret = this->subdirback->store->Close(fh);
    mlog(WF_DAPI, "%s:%s closed fh %p for %s: %d %s",
         __FILE__, __FUNCTION__, fh, path.c_str(), ret,
         ( ret != 0 ? strerror(-ret) : "success" ) );
    paths.erase ( fh );
    return ret;
}

// returns -err or number of writers
int
WriteFile::removeWriter( pid_t pid )
{
    int ret = 0;
    Util::MutexLock(   &data_mux , __FUNCTION__);
    struct OpenFh *ofh = getFh( pid );
    int writers = incrementOpens(-1);
    if ( ofh == NULL ) {
        // if we can't find it, we still decrement the writers count
        // this is strange but sometimes fuse does weird things w/ pids
        // if the writers goes zero, when this struct is freed, everything
        // gets cleaned up
        mlog(WF_CRIT, "%s can't find pid %d", __FUNCTION__, pid );
        assert( 0 );
    } else {
        ofh->writers--;
        if ( ofh->writers <= 0 ) {
            ret = closeFh( ofh->fh );
            fhs.erase( pid );
        }
    }
    mlog(WF_DAPI, "%s (%d) on %s now has %d writers: %d",
         __FUNCTION__, pid, container_path.c_str(), writers, ret );
    Util::MutexUnlock( &data_mux, __FUNCTION__ );
    return ( ret == 0 ? writers : ret );
}

int
WriteFile::extend( off_t offset )
{
	int dummy_chksm = 0; //added dummy checksum variable jacob
    // make a fake write
    if ( fhs.begin() == fhs.end() ) {
        return -ENOENT;
    }
    pid_t p = fhs.begin()->first;
    index->addWrite( offset, 0, p, createtime, createtime, dummy_chksm );//added dummy checksum parameter jacob );
    addWrite( offset, 0 );   // maintain metadata
    return 0;
}

// we are currently doing synchronous index writing.
// this is where to change it to buffer if you'd like
// We were thinking about keeping the buffer around the
// entire duration of the write, but that means our appended index will
// have a lot duplicate information. buffer the index and flush on the close
//
// returns bytes written or -err
ssize_t
WriteFile::write(const char *buf, size_t size, off_t offset, pid_t pid)
{
    int ret = 0;
    ssize_t written;
    int chksm = Container::hashValue(buf);//taking checksum of the buffer jacob
    OpenFh *ofh = getFh( pid );
    if ( ofh == NULL ) {
        // we used to return -ENOENT here but we can get here legitimately
        // when a parent opens a file and a child writes to it.
        // so when we get here, we need to add a child datafile
        ret = addWriter( pid, true );
        if ( ret > 0 ) {
            // however, this screws up the reference count
            // it looks like a new writer but it's multiple writers
            // sharing an fd ...
            ofh = getFh( pid );
        }
    }
    if ( ofh != NULL && ret >= 0 ) {
        IOSHandle *wfh = ofh->fh;
        // write the data file
        double begin, end;
        begin = Util::getTime();
        ret = written = ( size ? wfh->Write(buf, size ) : 0 );
        end = Util::getTime();
        // then the index
        if ( ret >= 0 ) {
            write_count++;
            Util::MutexLock(   &index_mux , __FUNCTION__);
            index->addWrite( offset, ret, pid, begin, end, chksm ); //added checksum as a new argument jacob
            // TODO: why is 1024 a magic number?
            int flush_count = 1024;
            if (write_count%flush_count==0) {
                ret = index->flush();
                // Check if the index has grown too large stop buffering
                if(index->memoryFootprintMBs() > index_buffer_mbs) {
                    index->stopBuffering();
                    mlog(WF_DCOMMON, "The index grew too large, "
                         "no longer buffering");
                }
            }
            if (ret >= 0) {
                addWrite(offset, size);    // track our own metadata
            }
            Util::MutexUnlock( &index_mux, __FUNCTION__ );
        }
    }
    // return bytes written or error
    return((ret >= 0) ? written : ret);
}

// this assumes that the hostdir exists and is full valid path
// returns 0 or -err
int WriteFile::openIndex( pid_t pid ) {
    int ret = 0;
    string index_path;
    /* note: this uses subdirback from obj to open */
    IOSHandle *fh = openIndexFile(subdir_path, hostname, pid, DROPPING_MODE,
                                  &index_path, ret);
    if ( fh != NULL ) {
        Util::MutexLock(&index_mux , __FUNCTION__);
        //XXXCDC:iostore need to pass the backend down into index?
        index = new Index(container_path, subdirback, fh);
        Util::MutexUnlock(&index_mux, __FUNCTION__);
        mlog(WF_DAPI, "In open Index path is %s",index_path.c_str());
        index->index_path=index_path;
        if(index_buffer_mbs) {
            index->startBuffering();
        }
    }
    return ret;
}

int WriteFile::closeIndex( )
{
    IOSHandle *closefh;
    struct plfs_backend *ib;
    int ret = 0;
    Util::MutexLock(   &index_mux , __FUNCTION__);
    ret = index->flush();
    closefh = index->getFh(&ib);
    /* XXX: a bit odd that we close the index instead of the index itself */
    //XXXCDC:iostore via ib
    ret = closeFh( closefh );
    delete( index );
    index = NULL;
    Util::MutexUnlock( &index_mux, __FUNCTION__ );
    return ret;
}

// returns 0 or -err
int WriteFile::Close()
{
    int failures = 0;
    Util::MutexLock(   &data_mux , __FUNCTION__);
    map<pid_t,OpenFh>::iterator itr;
    // these should already be closed here
    // from each individual pid's close but just in case
    for( itr = fhs.begin(); itr != fhs.end(); itr++ ) {
        if ( closeFh( itr->second.fh ) != 0 ) {
            failures++;
        }
    }
    fhs.clear();
    Util::MutexUnlock( &data_mux, __FUNCTION__ );
    return ( failures ? -EIO : 0 );
}

// returns 0 or -err
int WriteFile::truncate( off_t offset )
{
    Metadata::truncate( offset );
    index->truncateHostIndex( offset );
    return 0;
}

/* uses this->subdirback to open */
IOSHandle *WriteFile::openIndexFile(string path, string host, pid_t p,
                                    mode_t m, string *index_path, int &ret)
{
    *index_path = Container::getIndexPath(path,host,p,createtime);
    return openFile(*index_path,m,ret);
}

/* uses this->subdirback to open */
IOSHandle *WriteFile::openDataFile(string path, string host, pid_t p, mode_t m,
        int &ret)
{
    return openFile(Container::getDataPath(path,host,p,createtime),m,ret);
}

// returns an fh or null
IOSHandle *WriteFile::openFile(string physicalpath, mode_t xmode, int &ret )
{
    mode_t old_mode=umask(0);
    int flags = O_WRONLY | O_APPEND | O_CREAT;
    IOSHandle *fh;
    fh = this->subdirback->store->Open(physicalpath.c_str(), flags, xmode, ret);
    mlog(WF_DAPI, "%s.%s open %s : %p %s",
         __FILE__, __FUNCTION__,
         physicalpath.c_str(),
         fh, ( fh == NULL ? strerror(-ret) : "SUCCESS" ) );
    if ( fh != NULL ) {
        paths[fh] = physicalpath;    // remember so restore works
    }
    umask(old_mode);
    return(fh);
}

// we call this after any calls to f_truncate
// if fuse::f_truncate is used, we will have open handles that get messed up
// in that case, we need to restore them
// what if rename is called and then f_truncate?
// return 0 or -err
int WriteFile::restoreFds( bool droppings_were_truncd )
{
    map<IOSHandle *,string>::iterator paths_itr;
    map<pid_t, OpenFh >::iterator pids_itr;
    int ret = -ENOSYS;
    // "has_been_renamed" is set at "addWriter, setPath" executing path.
    // This assertion will be triggered when user open a file with write mode
    // and do truncate. Has nothing to do with upper layer rename so I comment
    // out this assertion but remain previous comments here.
    // if an open WriteFile ever gets truncated after being renamed, that
    // will be really tricky.  Let's hope that never happens, put an assert
    // to guard against it.  I guess it if does happen we just need to do
    // reg ex changes to all the paths
    //assert( ! has_been_renamed );
    mlog(WF_DAPI, "Entering %s",__FUNCTION__);
    // first reset the index fd
    if ( index ) {
        IOSHandle *restfh, *retfh;
        struct plfs_backend *ib;
        Util::MutexLock( &index_mux, __FUNCTION__ );
        index->flush();
        restfh = index->getFh(&ib);
        paths_itr = paths.find( restfh );
        if ( paths_itr == paths.end() ) {
            return -ENOENT;
        }
        string indexpath = paths_itr->second;
        /* note: this uses subdirback from object */
        if ( (ret=closeFh( restfh )) != 0 ) {
            return ret; 
        }
        /* note: this uses subdirback from object */
        if ( (retfh = openFile( indexpath, mode, ret )) < 0 ) {
            return ret; 
        }
        index->resetFh( retfh );
        if (droppings_were_truncd) {
            // this means that they were truncd to 0 offset
            index->resetPhysicalOffsets();
        }
        Util::MutexUnlock( &index_mux, __FUNCTION__ );
    }
    // then the data fds
    for( pids_itr = fhs.begin(); pids_itr != fhs.end(); pids_itr++ ) {
        paths_itr = paths.find( pids_itr->second.fh );
        if ( paths_itr == paths.end() ) {
            return -ENOENT;
        }
        string datapath = paths_itr->second;
        /* note: this uses subdirback from object */
        if ( (ret = closeFh( pids_itr->second.fh )) != 0 ) {
            return ret; 
        }
        /* note: this uses subdirback from object */
        pids_itr->second.fh = openFile( datapath, mode, ret );
        if ( pids_itr->second.fh == NULL ) {
            return ret; 
        }
    }
    // normally we return ret at the bottom of our functions but this
    // function had so much error handling, I just cut out early on any
    // error.  therefore, if we get here, it's happy days!
    mlog(WF_DAPI, "Exiting %s",__FUNCTION__);
    return 0;
}
