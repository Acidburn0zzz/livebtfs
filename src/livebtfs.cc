/*
Copyright 2015 Johan Gunnarsson <johan.gunnarsson@gmail.com>

This file is part of BTFS.

BTFS is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

BTFS is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with BTFS.  If not, see <http://www.gnu.org/licenses/>.
*/

#define FUSE_USE_VERSION 26

//#define _DEBUG

#include <cstdlib>
#include <iostream>
#include <fstream>

#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <fuse.h>

// The below pragma lines will silence lots of compiler warnings in the
// libtorrent headers file. Not livebtfs' fault.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/peer_request.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/version.hpp>
#if LIBTORRENT_VERSION_NUM >= 10200
#include <libtorrent/torrent_flags.hpp>
#endif
#pragma GCC diagnostic pop

#include <curl/curl.h>

#include "livebtfs.h"

#define RETV(s, v) { s; return v; };
#define STRINGIFY(s) #s

using namespace btfs;

libtorrent::session *session = NULL;

libtorrent::torrent_handle handle;

pthread_t alert_thread;

std::list<Read*> reads;

// First piece index of the current sliding window
//int cursor;

std::map<std::string,int> files;
std::map<std::string,std::set<std::string> > dirs;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t signal_cond = PTHREAD_COND_INITIALIZER;

// Time used as "last modified" time
time_t time_of_mount;

static struct btfs_params params;

Read::Read(char *buf, int index, off_t offset, size_t size) {
	auto ti = handle.torrent_file();

	int64_t file_size = ti->files().file_size(index);

	_size=0;

	while (size > 0 && offset < file_size) {
		libtorrent::peer_request part = ti->map_file(index, offset,
			(int) size);

		part.length = std::min(
			ti->piece_size(part.piece) - part.start,
			part.length);

		// bas : passer la priorite de la piece demandee de 0 a 7 (priorite la plus elevee)
		handle.piece_priority(part.piece,7);
		// bas
		
		parts.push_back(Part(part, buf));

		size -= (size_t) part.length;
		offset += part.length;
		buf += part.length;
	}
}

void Read::fail(int piece) {
	for (parts_iter i = parts.begin(); i != parts.end(); ++i) {
		if (i->part.piece == piece && !i->filled)
			failed = true;
	}
}

// return true if piece is found
bool Read::copy(int piece, char *buffer, int size) {
	for (parts_iter i = parts.begin(); i != parts.end(); ++i) {
		if (i->part.piece == piece )
		{
			if( !i->filled )
				i->filled = (memcpy(i->buf, buffer + i->part.start,
					(size_t) i->part.length)) != NULL;
			return true;
		}
	}
	return false;
}

void Read::seek_and_read (int numPiece) {
	for (parts_iter i = parts.begin(); i != parts.end(); ++i) {
		if ( i->part.piece == numPiece )
		{
			while ( ! i->filled && ! handle.have_piece(i->part.piece) );

			if ( ! i->filled )
				handle.read_piece(i->part.piece);
		}
		else
		{
			if ( ! i->filled )
				if (handle.have_piece(i->part.piece))
				{
					handle.read_piece(i->part.piece);
				}
		}
	}
}

void Read::trigger() {
	for (parts_iter i = parts.begin(); i != parts.end(); ++i) {
		if (handle.have_piece(i->part.piece))
			handle.read_piece(i->part.piece);
	}
}

bool Read::finished() {
	for (parts_iter i = parts.begin(); i != parts.end(); ++i) {
		if (!i->filled)
			return false;
	}

	return true;
}

int Read::size() {
	if ( _size ) // != 0 then already calculated
		return _size;

	for (parts_iter i = parts.begin(); i != parts.end(); ++i) {
		_size += i->part.length;
	}

	return _size;
}

int Read::read() {
	if (size() <= 0)
		return 0;

	// Trigger reads of finished pieces
	trigger();

	while (!finished() && !failed)
		// Wait for any piece to downloaded
		pthread_cond_wait(&signal_cond, &lock);

	if (failed)
		return -EIO;
	else
		return size();
}

static void
setup() {
	printf("Prototype version.\n");
	printf("Got metadata. Now ready to start downloading.\n");

	auto ti = handle.torrent_file();

	//bas : initialiser les priorites de telechargement des blocs a 0
	//i.e. pas de telechargement
	std::vector<int>::size_type  numpieces =  (std::vector<int>::size_type) ti->num_pieces();
	//std::vector<int> prios(numpieces); // default value is already : 0
	std::vector<int> prios(numpieces,0); // default value : 0
/*
	for (std::vector<int>::size_type i = 0 ; i < numpieces ; i++)
		prios[i]= 0 ;
*/
	handle.prioritize_pieces(prios);
	//bas

	if (params.browse_only)
		handle.pause();

	for (int i = 0; i < ti->num_files(); ++i) {
		std::string parent("");

		char *p = strdup(ti->files().file_path(i).c_str());

		if (!p)
			continue;

		for (char *x = strtok(p, "/"); x; x = strtok(NULL, "/")) {
			if (strlen(x) <= 0)
				continue;

			if (parent.length() <= 0)
				// Root dir <-> children mapping
				dirs["/"].insert(x);
			else
				// Non-root dir <-> children mapping
		 		dirs[parent].insert(x);

			parent += "/";
			parent += x;
		}

		free(p);

		// Path <-> file index mapping
		files["/" + ti->files().file_path(i)] = i;
	}
}

static void
handle_read_piece_alert(libtorrent::read_piece_alert *a, Log *log) {
	#ifdef _DEBUG
	printf("%s: piece %d size %d\n", __func__, static_cast<int>(a->piece),
		a->size);
	#endif

	pthread_mutex_lock(&lock);

	if (a->ec) {
		*log << a->message() << std::endl;

		for (reads_iter i = reads.begin(); i != reads.end(); ++i) {
			(*i)->fail(a->piece);
		}
	} else {
		for (reads_iter i = reads.begin(); i != reads.end(); ++i) {
			if ( (*i)->copy(a->piece, a->buffer.get(), a->size) )
				break;
		}
	}

	pthread_mutex_unlock(&lock);

	// Wake up all threads waiting for download
	pthread_cond_broadcast(&signal_cond);
}

static void
handle_piece_finished_alert(libtorrent::piece_finished_alert *a, Log *log) {

	int piece_to_found=static_cast<int>(a->piece_index);

	#ifdef _DEBUG
	printf("%s: %d\n", __func__, piece_to_found);
	#endif

	pthread_mutex_lock(&lock);

	for (reads_iter i = reads.begin(); i != reads.end(); ++i) {
		(*i)->seek_and_read(piece_to_found);
	}

	pthread_mutex_unlock(&lock);
}

static void
handle_torrent_added_alert(libtorrent::torrent_added_alert *a, Log *log) {
	pthread_mutex_lock(&lock);

	handle = a->handle;

	if (a->handle.status().has_metadata)
		setup();

	pthread_mutex_unlock(&lock);
}

static void
handle_metadata_received_alert(libtorrent::metadata_received_alert *a,
		Log *log) {
	pthread_mutex_lock(&lock);

	handle = a->handle;

	setup();

	pthread_mutex_unlock(&lock);
}

static void
handle_alert(libtorrent::alert *a, Log *log) {
	switch (a->type()) {
	case libtorrent::read_piece_alert::alert_type:
		handle_read_piece_alert(
			(libtorrent::read_piece_alert *) a, log);
		break;
	case libtorrent::piece_finished_alert::alert_type:
		*log << a->message() << std::endl;
		handle_piece_finished_alert(
			(libtorrent::piece_finished_alert *) a, log);
		break;
	case libtorrent::metadata_received_alert::alert_type:
		*log << a->message() << std::endl;
		handle_metadata_received_alert(
			(libtorrent::metadata_received_alert *) a, log);
		break;
	case libtorrent::torrent_added_alert::alert_type:
		*log << a->message() << std::endl;
		handle_torrent_added_alert(
			(libtorrent::torrent_added_alert *) a, log);
		break;
	case libtorrent::dht_bootstrap_alert::alert_type:
		*log << a->message() << std::endl;
		// Force DHT announce because libtorrent won't by itself
		handle.force_dht_announce();
		break;
	case libtorrent::dht_announce_alert::alert_type:
	case libtorrent::dht_reply_alert::alert_type:
	case libtorrent::metadata_failed_alert::alert_type:
	case libtorrent::tracker_announce_alert::alert_type:
	case libtorrent::tracker_reply_alert::alert_type:
	case libtorrent::tracker_warning_alert::alert_type:
	case libtorrent::tracker_error_alert::alert_type:
	case libtorrent::lsd_peer_alert::alert_type:
		*log << a->message() << std::endl;
		break;
	case libtorrent::stats_alert::alert_type:
		//*log << a->message() << std::endl;
		break;
	default:
		break;
	}

}


static void
alert_queue_loop_destroy(void *data) {
	Log *log = (Log *) data;

	if (log)
		delete log;
}

static void*
alert_queue_loop(void *data) {
	int oldstate, oldtype;

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);

	pthread_cleanup_push(&alert_queue_loop_destroy, data);

	while (1) {
		if (!session->wait_for_alert(libtorrent::seconds(1)))
			continue;

		std::vector<libtorrent::alert*> alerts;

		session->pop_alerts(&alerts);

		for (std::vector<libtorrent::alert*>::iterator i =
				alerts.begin(); i != alerts.end(); ++i) {
			handle_alert(*i, (Log *) data);
		}
	}

	pthread_cleanup_pop(1);

	return NULL;
}

static bool
is_root(const char *path) {
	return strcmp(path, "/") == 0;
}

static bool
is_dir(const char *path) {
	return dirs.find(path) != dirs.end();
}

static bool
is_file(const char *path) {
	return files.find(path) != files.end();
}

static int
btfs_getattr(const char *path, struct stat *stbuf) {
	if (!is_dir(path) && !is_file(path) && !is_root(path))
		return -ENOENT;

	pthread_mutex_lock(&lock);

	memset(stbuf, 0, sizeof (*stbuf));

	stbuf->st_uid = getuid();
	stbuf->st_gid = getgid();
	stbuf->st_mtime = time_of_mount;

	if (is_root(path) || is_dir(path)) {
		stbuf->st_mode = S_IFDIR | 0755;
	} else {
		auto ti = handle.torrent_file();

		int64_t file_size = ti->files().file_size(files[path]);

#if LIBTORRENT_VERSION_NUM < 10200
		std::vector<boost::int64_t> progress;
#else
		std::vector<std::int64_t> progress;
#endif

		// Get number of bytes downloaded of each file
		handle.file_progress(progress,
			libtorrent::torrent_handle::piece_granularity);

		stbuf->st_blocks = progress[(size_t) files[path]] / 512;
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_size = file_size;
	}

	pthread_mutex_unlock(&lock);

	return 0;
}

static int
btfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi) {
	if (!is_dir(path) && !is_file(path) && !is_root(path))
		return -ENOENT;

	if (is_file(path))
		return -ENOTDIR;

	pthread_mutex_lock(&lock);

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	for (std::set<std::string>::iterator i = dirs[path].begin();
			i != dirs[path].end(); ++i) {
		filler(buf, i->c_str(), NULL, 0);
	}

	pthread_mutex_unlock(&lock);

	return 0;
}

static int
btfs_open(const char *path, struct fuse_file_info *fi) {
	if (!is_dir(path) && !is_file(path))
		return -ENOENT;

	if (is_dir(path))
		return -EISDIR;

	if ((fi->flags & 3) != O_RDONLY)
		return -EACCES;

	return 0;
}

static int
btfs_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
	if (!is_dir(path) && !is_file(path))
		return -ENOENT;

	if (is_dir(path))
		return -EISDIR;

	if (params.browse_only)
		return -EACCES;

	pthread_mutex_lock(&lock);

	Read *r = new Read(buf, files[path], offset, size);

	reads.push_back(r);

	// Wait for read to finish
	int s = r->read();

	reads.remove(r);

	delete r;

	pthread_mutex_unlock(&lock);

	return s;
}

static int
btfs_statfs(const char *path, struct statvfs *stbuf) {
	libtorrent::torrent_status st = handle.status();

	if (!st.has_metadata)
		return -ENOENT;

	auto ti = handle.torrent_file();

	stbuf->f_bsize = 4096;
	stbuf->f_frsize = 512;
	stbuf->f_blocks = (fsblkcnt_t) (ti->total_size() / 512);
	stbuf->f_bfree = (fsblkcnt_t) ((ti->total_size() - st.total_done) / 512);
	stbuf->f_bavail = (fsblkcnt_t) ((ti->total_size() - st.total_done) / 512);
	stbuf->f_files = (fsfilcnt_t) (files.size() + dirs.size());
	stbuf->f_ffree = 0;

	return 0;
}

static void *
btfs_init(struct fuse_conn_info *conn) {
	pthread_mutex_lock(&lock);

	time_of_mount = time(NULL);

	libtorrent::add_torrent_params *p = (libtorrent::add_torrent_params *)
		fuse_get_context()->private_data;

#if LIBTORRENT_VERSION_NUM < 10200
	int flags =
#else
	libtorrent::session_flags_t flags =
#endif
		libtorrent::session::add_default_plugins |
		libtorrent::session::start_default_features;

#if LIBTORRENT_VERSION_NUM < 10200
	int alerts =
#else
	libtorrent::alert_category_t alerts =
#endif
		libtorrent::alert::tracker_notification |
		libtorrent::alert::stats_notification |
		libtorrent::alert::storage_notification |
		libtorrent::alert::progress_notification |
		libtorrent::alert::status_notification |
		libtorrent::alert::error_notification |
		libtorrent::alert::dht_notification |
		libtorrent::alert::peer_notification;

	libtorrent::settings_pack pack;

	std::ostringstream interfaces;

	// First port
	interfaces << "0.0.0.0:" << params.min_port;

	// Possibly more ports, but at most 5
	for (int i = params.min_port + 1; i <= params.max_port &&
			i < params.min_port + 5; i++)
		interfaces << ",0.0.0.0:" << i;

	std::string fingerprint =
		"LT"
		STRINGIFY(LIBTORRENT_VERSION_MAJOR)
		STRINGIFY(LIBTORRENT_VERSION_MINOR)
		"00";

	if ( params.disable_dht )
	{
		// disable DHT
		pack.set_str(libtorrent::settings_pack::dht_bootstrap_nodes, "");
		pack.set_bool(libtorrent::settings_pack::enable_dht, false);
	}
	else
	{
		// enable DHT
		pack.set_str(libtorrent::settings_pack::dht_bootstrap_nodes,
			"router.bittorrent.com:6881,"
			"router.utorrent.com:6881,"
			"dht.transmissionbt.com:6881");
	}

	pack.set_int(libtorrent::settings_pack::request_timeout, 10);
	//pack.set_int(libtorrent::settings_pack::request_timeout, 60);
	pack.set_int(libtorrent::settings_pack::peer_timeout, 10);

	pack.set_str(libtorrent::settings_pack::listen_interfaces, interfaces.str());
	pack.set_bool(libtorrent::settings_pack::strict_end_game_mode, false);
	pack.set_bool(libtorrent::settings_pack::announce_to_all_trackers, true);
	pack.set_bool(libtorrent::settings_pack::announce_to_all_tiers, true);
	pack.set_int(libtorrent::settings_pack::download_rate_limit, params.max_download_rate * 1024);
	pack.set_int(libtorrent::settings_pack::upload_rate_limit, params.max_upload_rate * 1024);
	pack.set_int(libtorrent::settings_pack::alert_mask, alerts);

	//pack.set_int(libtorrent::settings_pack::seed_choking_algorithm, libtorrent::settings_pack::fastest_upload);
	pack.set_int(libtorrent::settings_pack::seed_choking_algorithm, libtorrent::settings_pack::fixed_slots_choker);
	pack.set_int(libtorrent::settings_pack::unchoke_slots_limit, 30);
	//pack.set_int(libtorrent::settings_pack::unchoke_slots_limit, -1);

	pack.set_bool(libtorrent::settings_pack::prioritize_partial_pieces, true);
	pack.set_bool(libtorrent::settings_pack::close_redundant_connections, false);
	pack.set_bool(libtorrent::settings_pack::allow_multiple_connections_per_ip, true);

	session = new libtorrent::session(pack, flags);

	session->add_torrent(*p);

	pthread_create(&alert_thread, NULL, alert_queue_loop,
		new Log(p->save_path + "/../log.txt"));

#ifdef HAVE_PTHREAD_SETNAME_NP
	pthread_setname_np(alert_thread, "alert");
#endif

	pthread_mutex_unlock(&lock);

	return NULL;
}

static void
btfs_destroy(void *user_data) {
	pthread_mutex_lock(&lock);

	pthread_cancel(alert_thread);
	pthread_join(alert_thread, NULL);

#if LIBTORRENT_VERSION_NUM < 10200
	int flags = 0;
#else
	libtorrent::remove_flags_t flags = {};
#endif

	if (!params.keep)
		flags |= libtorrent::session::delete_files;

	session->remove_torrent(handle, flags);

	delete session;

	pthread_mutex_unlock(&lock);
}

static int
btfs_listxattr(const char *path, char *data, size_t len) {
	const char *xattrs = NULL;
	int xattrslen = 0;

	if (is_root(path)) {
		xattrs = XATTR_IS_BTFS "\0" XATTR_IS_BTFS_ROOT;
		xattrslen = sizeof (XATTR_IS_BTFS "\0" XATTR_IS_BTFS_ROOT);
	} else if (is_dir(path)) {
		xattrs = XATTR_IS_BTFS;
		xattrslen = sizeof (XATTR_IS_BTFS);
	} else if (is_file(path)) {
		xattrs = XATTR_IS_BTFS "\0" XATTR_FILE_INDEX;
		xattrslen = sizeof (XATTR_IS_BTFS "\0" XATTR_FILE_INDEX);
	} else {
		return -ENOENT;
	}

	// The minimum required length
	if (len == 0)
		return xattrslen;

	if (len < (size_t) xattrslen)
		return -ERANGE;

	memcpy(data, xattrs, (size_t) xattrslen);

	return xattrslen;
}

#ifdef __APPLE__
static int
btfs_getxattr(const char *path, const char *key, char *value, size_t len,
		uint32_t position) {
#else
static int
btfs_getxattr(const char *path, const char *key, char *value, size_t len) {
	uint32_t position = 0;
#endif
	char xattr[16];
	int xattrlen = 0;

	std::string k(key);

	if (is_file(path) && k == XATTR_FILE_INDEX) {
		xattrlen = snprintf(xattr, sizeof (xattr), "%d", files[path]);
	} else if (is_root(path) && k == XATTR_IS_BTFS_ROOT) {
		xattrlen = 0;
	} else if (k == XATTR_IS_BTFS) {
		xattrlen = 0;
	} else {
		return -ENODATA;
	}

	// The minimum required length
	if (len == 0)
		return xattrlen;

	if (position >= (uint32_t) xattrlen)
		return 0;

	if (len < (size_t) xattrlen - position)
		return -ERANGE;

	memcpy(value, xattr + position, (size_t) xattrlen - position);

	return xattrlen - (int) position;
}

static bool
populate_target(std::string& target, char *arg) {
	std::string templ;

	if (arg) {
		templ += arg;
	} else if (getenv("HOME")) {
		templ += getenv("HOME");
		templ += "/." PACKAGE;
	} else {
		templ += "/tmp/" PACKAGE;
	}

	if (mkdir(templ.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) < 0) {
		if (errno != EEXIST)
			RETV(perror("Failed to create target"), false);
	}

	templ += "/" PACKAGE "-XXXXXX";

	char *s = strdup(templ.c_str());

	if (s != NULL && mkdtemp(s) != NULL) {
		char *x = realpath(s, NULL);

		if (x)
			target = x;
		else
			perror("Failed to expand target");

		free(x);		
	} else {
		perror("Failed to generate target");
	}

	free(s);

	return target.length() > 0;
}

static size_t
handle_http(void *contents, size_t size, size_t nmemb, void *userp) {
	Array *output = (Array *) userp;

	// Offset into buffer to write to
	size_t off = output->size;

	output->expand(nmemb * size);

	memcpy(output->buf + off, contents, nmemb * size);

	// Must return number of bytes copied
	return nmemb * size;
}

static bool
populate_metadata(libtorrent::add_torrent_params& p, const char *arg) {
	std::string uri(arg);

	if (uri.find("http:") == 0 || uri.find("https:") == 0) {
		Array output;

		CURL *ch = curl_easy_init();

		curl_easy_setopt(ch, CURLOPT_URL, uri.c_str());
		curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, handle_http); 
		curl_easy_setopt(ch, CURLOPT_WRITEDATA, (void *) &output); 
		curl_easy_setopt(ch, CURLOPT_USERAGENT, PACKAGE "/" VERSION);
		curl_easy_setopt(ch, CURLOPT_FOLLOWLOCATION, 1);

		CURLcode res = curl_easy_perform(ch);

		if(res != CURLE_OK)
			RETV(fprintf(stderr, "Download metadata failed: %s\n",
				curl_easy_strerror(res)), false);

		curl_easy_cleanup(ch);

		libtorrent::error_code ec;

#if LIBTORRENT_VERSION_NUM < 10200
		p.ti = boost::make_shared<libtorrent::torrent_info>(
			(const char *) output.buf, (int) output.size,
			boost::ref(ec));
#else
		p.ti = std::make_shared<libtorrent::torrent_info>(
			(const char *) output.buf, (int) output.size,
			std::ref(ec));
#endif

		if (ec)
			RETV(fprintf(stderr, "Parse metadata failed: %s\n",
				ec.message().c_str()), false);

		if (params.browse_only)
#if LIBTORRENT_VERSION_NUM < 10200
			p.flags |= libtorrent::add_torrent_params::flag_paused;
#else
			p.flags |= libtorrent::torrent_flags::paused;
#endif
	} else if (uri.find("magnet:") == 0) {
		libtorrent::error_code ec;

		parse_magnet_uri(uri, p, ec);

		if (ec)
			RETV(fprintf(stderr, "Parse magnet failed: %s\n",
				ec.message().c_str()), false);
	} else {
		char *r = realpath(uri.c_str(), NULL);

		if (!r)
			RETV(perror("Find metadata failed"), false);

		libtorrent::error_code ec;

#if LIBTORRENT_VERSION_NUM < 10200
		p.ti = boost::make_shared<libtorrent::torrent_info>(r,
			boost::ref(ec));
#else
		p.ti = std::make_shared<libtorrent::torrent_info>(r,
			std::ref(ec));
#endif

		free(r);

		if (ec)
			RETV(fprintf(stderr, "Parse metadata failed: %s\n",
				ec.message().c_str()), false);

		if (params.browse_only)
#if LIBTORRENT_VERSION_NUM < 10200
			p.flags |= libtorrent::add_torrent_params::flag_paused;
#else
			p.flags |= libtorrent::torrent_flags::paused;
#endif
	}

	return true;
}

#define BTFS_OPT(t, p, v) { t, offsetof(struct btfs_params, p), v }

static const struct fuse_opt btfs_opts[] = {
	BTFS_OPT("-v",                           version,              1),
	BTFS_OPT("--version",                    version,              1),
	BTFS_OPT("-h",                           help,                 1),
	BTFS_OPT("--help",                       help,                 1),
	BTFS_OPT("--help-fuse",                  help_fuse,            1),
	BTFS_OPT("-b",                           browse_only,          1),
	BTFS_OPT("--browse-only",                browse_only,          1),
	BTFS_OPT("-k",                           keep,                 1),
	BTFS_OPT("--keep",                       keep,                 1),
	BTFS_OPT("--min-port=%lu",               min_port,             4),
	BTFS_OPT("--max-port=%lu",               max_port,             4),
	BTFS_OPT("--max-download-rate=%lu",      max_download_rate,    4),
	BTFS_OPT("--max-upload-rate=%lu",        max_upload_rate,      4),
	BTFS_OPT("--disable-dht",                disable_dht,          1),
	FUSE_OPT_END
};

static int
btfs_process_arg(void *data, const char *arg, int key,
		struct fuse_args *outargs) {
	// Number of NONOPT options so far
	static int n = 0;

	struct btfs_params *params = (struct btfs_params *) data;

	if (key == FUSE_OPT_KEY_NONOPT) {
		if (n++ == 0)
			params->metadata = arg;

		return n <= 1 ? 0 : 1;
	}

	return 1;
}

static void
print_help() {
	printf("usage: " PACKAGE " [options] metadata mountpoint\n");
	printf("\n");
	printf(PACKAGE " options:\n");
	printf("    --version -v           show version information\n");
	printf("    --help -h              show this message\n");
	printf("    --help-fuse            print all fuse options\n");
	printf("    --browse-only -b       download metadata only\n");
	printf("    --keep -k              keep files after unmount\n");
	printf("    --min-port=N           start of listen port range\n");
	printf("    --max-port=N           end of listen port range\n");
	printf("    --max-download-rate=N  max download rate (in kB/s)\n");
	printf("    --max-upload-rate=N    max upload rate (in kB/s)\n");
	printf("    --disable-dht          disable the usage of DHT\n");
}

int
main(int argc, char *argv[]) {
	struct fuse_operations btfs_ops;
	memset(&btfs_ops, 0, sizeof (btfs_ops));

	btfs_ops.getattr = btfs_getattr;
	btfs_ops.readdir = btfs_readdir;
	btfs_ops.open = btfs_open;
	btfs_ops.read = btfs_read;
	btfs_ops.statfs = btfs_statfs;
	btfs_ops.listxattr = btfs_listxattr;
	btfs_ops.getxattr = btfs_getxattr;
	btfs_ops.init = btfs_init;
	btfs_ops.destroy = btfs_destroy;

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	if (fuse_opt_parse(&args, &params, btfs_opts, btfs_process_arg))
		RETV(fprintf(stderr, "Failed to parse options\n"), -1);

	if (!params.metadata)
		params.help = 1;

	if (params.version) {
		printf(PACKAGE " version: " VERSION "\n");
		printf("libtorrent version: " LIBTORRENT_VERSION "\n");

		// Let FUSE print more versions
		fuse_opt_add_arg(&args, "--version");
		fuse_main(args.argc, args.argv, &btfs_ops, NULL);

		return 0;
	}

	if (params.help || params.help_fuse) {
		// Print info about livebtfs' command line options
		print_help();

		if (params.help_fuse) {
			printf("\n");

			// Let FUSE print more help
			fuse_opt_add_arg(&args, "-ho");
			fuse_main(args.argc, args.argv, &btfs_ops, NULL);
		}

		return 0;
	}

	if (params.min_port == 0 && params.max_port == 0) {
		// Default ports are the standard Bittorrent range
		params.min_port = 6881;
		params.max_port = 6889;
	} else if (params.min_port == 0) {
		params.min_port = 1024;
	} else if (params.max_port == 0) {
		params.max_port = 65535;
	}

	if (params.min_port > params.max_port)
		RETV(fprintf(stderr, "Invalid port range\n"), -1);

	std::string target;

	if (!populate_target(target, NULL))
		return -1;

	libtorrent::add_torrent_params p;

#if LIBTORRENT_VERSION_NUM < 10200
	p.flags &= ~libtorrent::add_torrent_params::flag_auto_managed;
	p.flags &= ~libtorrent::add_torrent_params::flag_paused;
#else
	p.flags &= ~libtorrent::torrent_flags::auto_managed;
	p.flags &= ~libtorrent::torrent_flags::paused;
#endif
	p.save_path = target + "/files";

	if (mkdir(p.save_path.c_str(), 0777) < 0)
		RETV(perror("Failed to create files directory"), -1);

	curl_global_init(CURL_GLOBAL_ALL);

	if (!populate_metadata(p, params.metadata))
		return -1;

	fuse_main(args.argc, args.argv, &btfs_ops, (void *) &p);

	curl_global_cleanup();

	if (!params.keep) {
		if (rmdir(p.save_path.c_str()))
			RETV(perror("Failed to remove files directory"), -1);

		if (rmdir(target.c_str()))
			RETV(perror("Failed to remove target directory"), -1);
	}

	return 0;
}