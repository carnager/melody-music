package main

import (
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// commandTable maps MPD command names to handler functions.
var commandTable map[string]func(*mpdConn, []string) *mpdError

func init() {
	commandTable = map[string]func(*mpdConn, []string) *mpdError{
	// Status
	"status":      cmdStatus,
	"currentsong": cmdCurrentSong,
	"stats":       cmdStats,

	// Playback control
	"play":     cmdPlay,
	"playid":   cmdPlayID,
	"pause":    cmdPause,
	"stop":     cmdStop,
	"next":     cmdNext,
	"previous": cmdPrevious,
	"seekcur":  cmdSeekCur,
	"seek":     cmdSeek,
	"seekid":   cmdSeekID,

	// Queue
	"playlistinfo": cmdPlaylistInfo,
	"playlistid":   cmdPlaylistID,
	"plchanges":    cmdPlChanges,
	"add":          cmdAdd,
	"addid":        cmdAddID,
	"delete":       cmdDelete,
	"deleteid":     cmdDeleteID,
	"clear":        cmdClear,
	"move":         cmdMove,
	"moveid":       cmdMoveID,
	"shuffle":      cmdShuffle,

	// Database
	"lsinfo":  cmdLsInfo,
	"list":    cmdList,
	"find":    cmdFind,
	"search":  cmdSearch,
	"count":   cmdCount,
	"listall": cmdListAll,
	"listallinfo": cmdListAllInfo,
	"findadd":     cmdFindAdd,
	"searchadd":   cmdSearchAdd,

	// Stored playlists
	"listplaylists":     cmdListPlaylists,
	"listplaylistinfo":  cmdListPlaylistInfo,
	"load":              cmdLoad,
	"save":              cmdSave,
	"rm":                cmdRm,
	"playlistadd":       cmdPlaylistAdd,

	// Outputs (devices)
	"outputs":       cmdOutputs,
	"enableoutput":  cmdEnableOutput,
	"disableoutput": cmdDisableOutput,

	// Volume
	"setvol":   cmdSetVol,
	"volume":   cmdVolume,

	// Options
	"replay_gain_mode":   cmdReplayGainMode,
	"replay_gain_status": cmdReplayGainStatus,
	"repeat":             cmdIgnore,
	"random":             cmdIgnore,
	"single":             cmdIgnore,
	"consume":            cmdIgnore,
	"crossfade":          cmdIgnore,

	// Ratings (custom extension)
	"rate":           cmdRate,
	"albumrate":      cmdAlbumRate,
	"getrating":      cmdGetRating,
	"getalbumrating": cmdGetAlbumRating,

	// Cover art
	"albumart":    cmdAlbumArt,
	"readpicture": cmdReadPicture,

	// Connection
	"ping":         cmdPing,
	"commands":     cmdCommands,
	"notcommands":  cmdNotCommands,
	"tagtypes":     cmdTagTypes,
	"decoders":     cmdDecoders,
	"binarylimit":  cmdIgnore,
	"password":     cmdIgnore,
	"config":       cmdIgnore,
	"urlhandlers":  cmdIgnore,
	"protocols":    cmdIgnore,
	"subscribe":    cmdIgnore,
	"unsubscribe":  cmdIgnore,
	"channels":     cmdChannels,
	"readmessages": cmdEmpty,
	"mixrampdb":    cmdIgnore,
	"mixrampdelay": cmdIgnore,
	}
}

// ---------------------------------------------------------------------------
// Status commands
// ---------------------------------------------------------------------------

func cmdStatus(c *mpdConn, args []string) *mpdError {
	a := c.app
	t := a.target()

	state := "stop"
	var elapsed, duration float64
	var songPos int = -1
	var volume int = -1

	if t.isRunning() {
		if pauseRaw, err := t.getProperty("pause"); err == nil {
			if p, ok := pauseRaw.(bool); ok {
				if p {
					state = "pause"
				} else {
					state = "play"
				}
			}
		}
		if posRaw, err := t.getProperty("playlist-pos"); err == nil {
			if f, ok := posRaw.(float64); ok {
				songPos = int(f)
			}
		}
		if tpRaw, err := t.getProperty("time-pos"); err == nil {
			if f, ok := tpRaw.(float64); ok {
				elapsed = f
			}
		}
		if durRaw, err := t.getProperty("duration"); err == nil {
			if f, ok := durRaw.(float64); ok {
				duration = f
			}
		}
		if volRaw, err := t.getProperty("volume"); err == nil {
			if f, ok := volRaw.(float64); ok {
				volume = int(f)
			}
		}
	}

	a.playQueueMu.Lock()
	queueLen := len(a.playQueue)
	queueVer := a.queueVersion
	a.playQueueMu.Unlock()

	if volume >= 0 {
		c.writeKV("volume", volume)
	} else {
		c.writeKV("volume", -1)
	}
	c.writeKV("repeat", 0)
	c.writeKV("random", 0)
	c.writeKV("single", 0)
	c.writeKV("consume", 0)
	c.writeKV("playlist", queueVer)
	c.writeKV("playlistlength", queueLen)
	c.writeKV("state", state)
	if songPos >= 0 {
		c.writeKV("song", songPos)
		a.playQueueMu.Lock()
		if songPos < len(a.queueIDs) {
			c.writeKV("songid", a.queueIDs[songPos])
		}
		a.playQueueMu.Unlock()
		c.writef("elapsed: %.3f\n", elapsed)
		c.writef("duration: %.3f\n", duration)
		c.writeKV("time", fmt.Sprintf("%d:%d", int(elapsed), int(duration)))
	}
	if songPos+1 < queueLen {
		c.writeKV("nextsong", songPos+1)
		a.playQueueMu.Lock()
		if songPos+1 < len(a.queueIDs) {
			c.writeKV("nextsongid", a.queueIDs[songPos+1])
		}
		a.playQueueMu.Unlock()
	}
	return nil
}

func cmdCurrentSong(c *mpdConn, args []string) *mpdError {
	a := c.app
	songID := a.currentPlayingSongID()
	if songID == "" {
		return nil // no current song, return empty OK
	}
	track := a.findTrackBySongID(songID)
	if track == nil {
		return nil
	}

	a.playQueueMu.Lock()
	pos := -1
	mpdID := 0
	for i, id := range a.playQueue {
		if id == songID {
			pos = i
			if i < len(a.queueIDs) {
				mpdID = a.queueIDs[i]
			}
			break
		}
	}
	a.playQueueMu.Unlock()

	c.writeTrack(track, pos, mpdID)
	return nil
}

func cmdStats(c *mpdConn, args []string) *mpdError {
	trackCount, _ := c.app.db.trackCount()
	albumCount, _ := c.app.db.albumCount()
	artists, _ := c.app.db.allArtists()
	c.writeKV("artists", len(artists))
	c.writeKV("albums", albumCount)
	c.writeKV("songs", trackCount)
	c.writeKV("uptime", 0)
	c.writeKV("db_playtime", 0)
	c.writeKV("db_update", 0)
	c.writeKV("playtime", 0)
	return nil
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

func cmdPlay(c *mpdConn, args []string) *mpdError {
	t := c.app.target()
	if len(args) > 0 {
		pos, err := strconv.Atoi(args[0])
		if err != nil {
			return mpdErr(errArg, "play", "invalid position")
		}
		if err := t.setProperty("playlist-pos", pos); err != nil {
			return mpdErr(errSystem, "play", err.Error())
		}
	} else {
		// No position arg: if nothing is playing, start from the beginning
		posRaw, err := t.getProperty("playlist-pos")
		if err != nil || posRaw == nil {
			_ = t.setProperty("playlist-pos", 0)
		} else if f, ok := posRaw.(float64); ok && f < 0 {
			_ = t.setProperty("playlist-pos", 0)
		}
	}
	if err := t.setProperty("pause", false); err != nil {
		return mpdErr(errSystem, "play", err.Error())
	}
	c.app.mpdHub.notify(SubPlayer)
	return nil
}

func cmdPlayID(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return cmdPlay(c, nil)
	}
	mpdID, err := strconv.Atoi(args[0])
	if err != nil {
		return mpdErr(errArg, "playid", "invalid id")
	}
	pos := c.app.queuePosByMPDID(mpdID)
	if pos < 0 {
		return mpdErr(errNoExist, "playid", "song not found")
	}
	return cmdPlay(c, []string{strconv.Itoa(pos)})
}

func cmdPause(c *mpdConn, args []string) *mpdError {
	t := c.app.target()
	if len(args) > 0 {
		val := args[0] == "1"
		if err := t.setProperty("pause", val); err != nil {
			return mpdErr(errSystem, "pause", err.Error())
		}
	} else {
		// Toggle
		pauseRaw, _ := t.getProperty("pause")
		paused, _ := pauseRaw.(bool)
		if err := t.setProperty("pause", !paused); err != nil {
			return mpdErr(errSystem, "pause", err.Error())
		}
	}
	c.app.mpdHub.notify(SubPlayer)
	return nil
}

func cmdStop(c *mpdConn, args []string) *mpdError {
	t := c.app.target()
	_ = t.setProperty("pause", true)
	c.app.mpdHub.notify(SubPlayer)
	return nil
}

func cmdNext(c *mpdConn, args []string) *mpdError {
	t := c.app.target()
	if _, err := t.command("playlist-next"); err != nil {
		return mpdErr(errSystem, "next", err.Error())
	}
	c.app.mpdHub.notify(SubPlayer)
	return nil
}

func cmdPrevious(c *mpdConn, args []string) *mpdError {
	t := c.app.target()
	if _, err := t.command("playlist-prev"); err != nil {
		return mpdErr(errSystem, "previous", err.Error())
	}
	c.app.mpdHub.notify(SubPlayer)
	return nil
}

func cmdSeekCur(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "seekcur", "need time argument")
	}
	t := c.app.target()
	timeStr := args[0]
	if strings.HasPrefix(timeStr, "+") || strings.HasPrefix(timeStr, "-") {
		// Relative seek
		offset, err := strconv.ParseFloat(timeStr, 64)
		if err != nil {
			return mpdErr(errArg, "seekcur", "invalid time")
		}
		posRaw, _ := t.getProperty("time-pos")
		pos, _ := posRaw.(float64)
		if err := t.setProperty("time-pos", pos+offset); err != nil {
			return mpdErr(errSystem, "seekcur", err.Error())
		}
	} else {
		pos, err := strconv.ParseFloat(timeStr, 64)
		if err != nil {
			return mpdErr(errArg, "seekcur", "invalid time")
		}
		if err := t.setProperty("time-pos", pos); err != nil {
			return mpdErr(errSystem, "seekcur", err.Error())
		}
	}
	c.app.mpdHub.notify(SubPlayer)
	return nil
}

func cmdSeek(c *mpdConn, args []string) *mpdError {
	if len(args) < 2 {
		return mpdErr(errArg, "seek", "need position and time arguments")
	}
	pos, err := strconv.Atoi(args[0])
	if err != nil {
		return mpdErr(errArg, "seek", "invalid position")
	}
	timePos, err := strconv.ParseFloat(args[1], 64)
	if err != nil {
		return mpdErr(errArg, "seek", "invalid time")
	}
	t := c.app.target()
	if err := t.setProperty("playlist-pos", pos); err != nil {
		return mpdErr(errSystem, "seek", err.Error())
	}
	if err := t.setProperty("time-pos", timePos); err != nil {
		return mpdErr(errSystem, "seek", err.Error())
	}
	c.app.mpdHub.notify(SubPlayer)
	return nil
}

func cmdSeekID(c *mpdConn, args []string) *mpdError {
	if len(args) < 2 {
		return mpdErr(errArg, "seekid", "need songid and time arguments")
	}
	mpdID, err := strconv.Atoi(args[0])
	if err != nil {
		return mpdErr(errArg, "seekid", "invalid id")
	}
	pos := c.app.queuePosByMPDID(mpdID)
	if pos < 0 {
		return mpdErr(errNoExist, "seekid", "song not found")
	}
	return cmdSeek(c, []string{strconv.Itoa(pos), args[1]})
}

// ---------------------------------------------------------------------------
// Queue commands
// ---------------------------------------------------------------------------

func cmdPlaylistInfo(c *mpdConn, args []string) *mpdError {
	a := c.app
	a.playQueueMu.Lock()
	queue := make([]string, len(a.playQueue))
	copy(queue, a.playQueue)
	ids := make([]int, len(a.queueIDs))
	copy(ids, a.queueIDs)
	a.playQueueMu.Unlock()

	start, end := 0, len(queue)
	if len(args) > 0 {
		s, e, err := parseRange(args[0], len(queue))
		if err != nil {
			return mpdErr(errArg, "playlistinfo", err.Error())
		}
		start, end = s, e
	}

	for i := start; i < end; i++ {
		track := a.findTrackBySongID(queue[i])
		if track == nil {
			continue
		}
		mpdID := 0
		if i < len(ids) {
			mpdID = ids[i]
		}
		c.writeTrack(track, i, mpdID)
	}
	return nil
}

func cmdPlaylistID(c *mpdConn, args []string) *mpdError {
	a := c.app
	if len(args) == 0 {
		return cmdPlaylistInfo(c, nil)
	}
	mpdID, err := strconv.Atoi(args[0])
	if err != nil {
		return mpdErr(errArg, "playlistid", "invalid id")
	}
	pos := a.queuePosByMPDID(mpdID)
	if pos < 0 {
		return mpdErr(errNoExist, "playlistid", "song not found")
	}
	return cmdPlaylistInfo(c, []string{strconv.Itoa(pos)})
}

func cmdPlChanges(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "plchanges", "need version argument")
	}
	ver, err := strconv.Atoi(args[0])
	if err != nil {
		return mpdErr(errArg, "plchanges", "invalid version")
	}
	c.app.playQueueMu.Lock()
	currentVer := c.app.queueVersion
	c.app.playQueueMu.Unlock()

	if ver >= currentVer {
		return nil // no changes
	}
	// Return full playlist on any version mismatch
	return cmdPlaylistInfo(c, nil)
}

func cmdAdd(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "add", "need URI argument")
	}
	uri := args[0]
	a := c.app

	// Try as single file first
	absPath := filepath.Join(a.cfg.Library.MusicDir, uri)
	track, err := a.db.trackByPath(absPath)
	if err == nil {
		songID := stringify(track["song_id"])
		if err := a.addSongsToPlaylist([]string{songID}, "add"); err != nil {
			return mpdErr(errSystem, "add", err.Error())
		}
		a.mpdHub.notify(SubPlaylist)
		return nil
	}

	// Try as directory prefix (add all tracks under this path)
	dirPath := absPath + "/"
	tracks, err := a.db.tracksByPathPrefix(dirPath)
	if err != nil || len(tracks) == 0 {
		return mpdErr(errNoExist, "add", "not found in database")
	}
	songIDs := make([]string, len(tracks))
	for i, t := range tracks {
		songIDs[i] = stringify(t["song_id"])
	}
	if err := a.addSongsToPlaylist(songIDs, "add"); err != nil {
		return mpdErr(errSystem, "add", err.Error())
	}
	a.mpdHub.notify(SubPlaylist)
	return nil
}

func cmdAddID(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "addid", "need URI argument")
	}
	uri := args[0]
	a := c.app

	absPath := filepath.Join(a.cfg.Library.MusicDir, uri)
	track, err := a.db.trackByPath(absPath)
	if err != nil {
		return mpdErr(errNoExist, "addid", "not found in database")
	}
	songID := stringify(track["song_id"])

	// If position given, use insert mode
	mode := "add"
	if len(args) > 1 {
		// MPD addid with position — we add then move
		mode = "add"
	}
	if err := a.addSongsToPlaylist([]string{songID}, mode); err != nil {
		return mpdErr(errSystem, "addid", err.Error())
	}

	// Get the newly assigned MPD ID
	a.playQueueMu.Lock()
	newMPDID := 0
	if len(a.queueIDs) > 0 {
		newMPDID = a.queueIDs[len(a.queueIDs)-1]
	}

	// If position was specified, move from end to requested position
	if len(args) > 1 {
		targetPos, parseErr := strconv.Atoi(args[1])
		if parseErr == nil && targetPos < len(a.playQueue)-1 {
			from := len(a.playQueue) - 1
			a.target().playlistMove(from, targetPos)
			// Update playQueue and queueIDs
			entry := a.playQueue[from]
			entryID := a.queueIDs[from]
			a.playQueue = append(a.playQueue[:from], a.playQueue[from+1:]...)
			a.queueIDs = append(a.queueIDs[:from], a.queueIDs[from+1:]...)
			// Insert at target
			a.playQueue = append(a.playQueue[:targetPos], append([]string{entry}, a.playQueue[targetPos:]...)...)
			a.queueIDs = append(a.queueIDs[:targetPos], append([]int{entryID}, a.queueIDs[targetPos:]...)...)
			a.savePlayQueue()
			newMPDID = entryID
		}
	}
	a.playQueueMu.Unlock()

	c.writeKV("Id", newMPDID)
	a.mpdHub.notify(SubPlaylist)
	return nil
}

func cmdDelete(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "delete", "need position argument")
	}
	a := c.app
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()

	start, end, err := parseRange(args[0], len(a.playQueue))
	if err != nil {
		return mpdErr(errArg, "delete", err.Error())
	}

	t := a.target()
	// Delete from end to start to preserve indices
	for i := end - 1; i >= start; i-- {
		_ = t.playlistRemove(i)
	}
	a.playQueue = append(a.playQueue[:start], a.playQueue[end:]...)
	a.queueIDs = append(a.queueIDs[:start], a.queueIDs[end:]...)
	a.queueVersion++
	a.savePlayQueue()
	a.mpdHub.notify(SubPlaylist)
	return nil
}

func cmdDeleteID(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "deleteid", "need songid argument")
	}
	mpdID, err := strconv.Atoi(args[0])
	if err != nil {
		return mpdErr(errArg, "deleteid", "invalid id")
	}
	pos := c.app.queuePosByMPDID(mpdID)
	if pos < 0 {
		return mpdErr(errNoExist, "deleteid", "song not found")
	}
	return cmdDelete(c, []string{strconv.Itoa(pos)})
}

func cmdClear(c *mpdConn, args []string) *mpdError {
	a := c.app
	a.playQueueMu.Lock()
	t := a.target()
	// mpv's playlist-clear keeps the current entry, so stop + clear + remove
	_ = t.setProperty("pause", true)
	_ = t.playlistClear()
	_ = t.playlistRemove(0) // remove the still-current entry
	a.playQueue = nil
	a.queueIDs = nil
	a.queueVersion++
	a.savePlayQueue()
	a.playQueueMu.Unlock()
	a.mpdHub.notify(SubPlaylist, SubPlayer)
	return nil
}

func cmdMove(c *mpdConn, args []string) *mpdError {
	if len(args) < 2 {
		return mpdErr(errArg, "move", "need from and to arguments")
	}
	a := c.app
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()

	from, err := strconv.Atoi(args[0])
	if err != nil || from < 0 || from >= len(a.playQueue) {
		return mpdErr(errArg, "move", "invalid from position")
	}
	to, err := strconv.Atoi(args[1])
	if err != nil || to < 0 || to >= len(a.playQueue) {
		return mpdErr(errArg, "move", "invalid to position")
	}

	_ = a.target().playlistMove(from, to)

	entry := a.playQueue[from]
	entryID := a.queueIDs[from]
	a.playQueue = append(a.playQueue[:from], a.playQueue[from+1:]...)
	a.queueIDs = append(a.queueIDs[:from], a.queueIDs[from+1:]...)
	if to > from {
		to--
	}
	a.playQueue = append(a.playQueue[:to], append([]string{entry}, a.playQueue[to:]...)...)
	a.queueIDs = append(a.queueIDs[:to], append([]int{entryID}, a.queueIDs[to:]...)...)
	a.queueVersion++
	a.savePlayQueue()
	a.mpdHub.notify(SubPlaylist)
	return nil
}

func cmdMoveID(c *mpdConn, args []string) *mpdError {
	if len(args) < 2 {
		return mpdErr(errArg, "moveid", "need songid and to arguments")
	}
	mpdID, err := strconv.Atoi(args[0])
	if err != nil {
		return mpdErr(errArg, "moveid", "invalid id")
	}
	pos := c.app.queuePosByMPDID(mpdID)
	if pos < 0 {
		return mpdErr(errNoExist, "moveid", "song not found")
	}
	return cmdMove(c, []string{strconv.Itoa(pos), args[1]})
}

func cmdShuffle(c *mpdConn, args []string) *mpdError {
	// Not implemented — return OK as no-op
	return nil
}

// ---------------------------------------------------------------------------
// Database commands
// ---------------------------------------------------------------------------

func cmdLsInfo(c *mpdConn, args []string) *mpdError {
	a := c.app
	uri := ""
	if len(args) > 0 {
		uri = args[0]
	}
	uri = strings.TrimSuffix(uri, "/")

	absDir := a.cfg.Library.MusicDir
	if uri != "" && uri != "/" {
		absDir = filepath.Join(a.cfg.Library.MusicDir, uri)
	}

	entries, err := os.ReadDir(absDir)
	if err != nil {
		return mpdErr(errNoExist, "lsinfo", "no such directory")
	}

	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".") {
			continue
		}
		entryURI := name
		if uri != "" && uri != "/" {
			entryURI = uri + "/" + name
		}
		if entry.IsDir() {
			c.writeKV("directory", entryURI)
		} else {
			// Check if this file is a known track in the DB
			absPath := filepath.Join(absDir, name)
			track, err := a.db.trackByPath(absPath)
			if err == nil {
				c.writeTrack(track, -1, 0)
			}
			// Skip non-music files silently
		}
	}
	return nil
}

func cmdList(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "list", "need type argument")
	}
	a := c.app
	tagType := strings.ToLower(args[0])

	// Parse optional filter and group clauses
	// Forms:
	//   list Album AlbumArtist "Name"
	//   list Album AlbumArtist "Name" group Date
	//   list Album group AlbumArtist group Date
	//   list Album "(AlbumArtist == \"Name\")" group Date
	filterTag, filterVal := "", ""
	var groupTags []string
	i := 1
	// Check for new-style filter expression
	if i < len(args) && strings.HasPrefix(args[i], "(") {
		conditions := parseFilterExpr(args[i])
		if len(conditions) > 0 {
			filterTag = conditions[0].tag
			filterVal = conditions[0].value
		}
		i++
	} else if i+1 < len(args) && strings.ToLower(args[i]) != "group" {
		// Old-style: tag value
		filterTag = strings.ToLower(args[i])
		filterVal = args[i+1]
		i += 2
	}
	// Parse group clauses
	for i+1 < len(args) {
		if strings.ToLower(args[i]) == "group" {
			groupTags = append(groupTags, strings.ToLower(args[i+1]))
			i += 2
		} else {
			i++
		}
	}

	groupSet := map[string]bool{}
	for _, g := range groupTags {
		groupSet[g] = true
	}

	switch tagType {
	case "artist", "albumartist":
		artists, err := a.db.allArtists()
		if err != nil {
			return mpdErr(errSystem, "list", err.Error())
		}
		key := "Artist"
		if tagType == "albumartist" {
			key = "AlbumArtist"
		}
		for _, name := range artists {
			c.writeKV(key, name)
		}
	case "album":
		var albums []map[string]any
		var err error
		if filterTag == "artist" || filterTag == "albumartist" {
			albums, err = a.db.albumsByArtist(filterVal)
		} else {
			albums, err = a.db.allAlbums(false)
		}
		if err != nil {
			return mpdErr(errSystem, "list", err.Error())
		}
		for _, album := range albums {
			// Write grouped tags before the main tag (MPD convention)
			if groupSet["albumartist"] {
				c.writeKV("AlbumArtist", stringify(album["albumartist"]))
			}
			if groupSet["date"] {
				c.writeKV("Date", stringify(album["date"]))
			}
			c.writeKV("Album", stringify(album["album"]))
			// Custom extension for melody clients
			if v := stringify(album["album_id"]); v != "" {
				c.writeKV("X-AlbumId", v)
			}
		}
	case "title":
		tracks, err := a.db.allTracks()
		if err != nil {
			return mpdErr(errSystem, "list", err.Error())
		}
		for _, track := range tracks {
			c.writeKV("Title", stringify(track["title"]))
		}
	case "date":
		albums, err := a.db.allAlbums(false)
		if err != nil {
			return mpdErr(errSystem, "list", err.Error())
		}
		seen := map[string]bool{}
		for _, album := range albums {
			d := stringify(album["date"])
			if d != "" && d != "0000" && !seen[d] {
				seen[d] = true
				c.writeKV("Date", d)
			}
		}
	default:
		return mpdErr(errArg, "list", "unsupported tag type: "+tagType)
	}
	return nil
}

func cmdFind(c *mpdConn, args []string) *mpdError {
	return cmdSearchOrFind(c, args, "find", false)
}

func cmdSearch(c *mpdConn, args []string) *mpdError {
	return cmdSearchOrFind(c, args, "search", true)
}

func cmdSearchOrFind(c *mpdConn, args []string, cmdName string, caseInsensitive bool) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, cmdName, "need filter arguments")
	}
	return cmdSearchOrFindInner(c, args, cmdName, caseInsensitive, false)
}

func cmdSearchOrFindInner(c *mpdConn, args []string, cmdName string, caseInsensitive, addToQueue bool) *mpdError {
	// Check if all args are filter expressions (start with paren)
	allFilters := true
	for _, a := range args {
		if !strings.HasPrefix(a, "(") {
			allFilters = false
			break
		}
	}

	if allFilters {
		// Collect conditions from all filter expression args
		// Real MPD: find "(AlbumArtist == \"x\")" "(Album == \"y\")" = AND of all
		var conditions []filterCondition
		for _, a := range args {
			conditions = append(conditions, parseFilterExpr(a)...)
		}
		if len(conditions) > 0 {
			return cmdFindByConditions(c, conditions, cmdName, caseInsensitive, addToQueue)
		}
		return nil
	}

	// Old-style: tag value [tag value ...]
	if len(args) < 2 {
		return mpdErr(errArg, cmdName, "need filter arguments")
	}

	conditions := oldStyleToConditions(args)
	if len(conditions) > 0 {
		return cmdFindByConditions(c, conditions, cmdName, caseInsensitive, addToQueue)
	}

	// Fallback: text search
	query := ""
	for i := 1; i < len(args); i += 2 {
		if query != "" {
			query += " "
		}
		query += args[i]
	}
	return cmdTextSearch(c, query, nil, nil, cmdName, addToQueue)
}

type ratingCond struct {
	op    string // "==", ">", ">=", "<", "<="
	value int
}

func (rc *ratingCond) matches(r int) bool {
	switch rc.op {
	case "==":
		return r == rc.value
	case ">":
		return r > rc.value
	case ">=":
		return r >= rc.value
	case "<":
		return r < rc.value
	case "<=":
		return r <= rc.value
	}
	return false
}

// sqlOp returns the SQL comparison operator and value for use in WHERE clauses.
func (rc *ratingCond) sqlOp() (string, int) {
	op := rc.op
	if op == "==" {
		op = "="
	}
	return op, rc.value
}

// cmdFindByConditions resolves a set of filter conditions against the database.
func cmdFindByConditions(c *mpdConn, conditions []filterCondition, cmdName string, caseInsensitive, addToQueue bool) *mpdError {
	a := c.app

	// Extract rating filters before building tag map
	var ratingFilter *ratingCond
	var albumRatingFilter *ratingCond
	var filteredConditions []filterCondition
	for _, cond := range conditions {
		switch cond.tag {
		case "rating", "x-rating":
			v, _ := strconv.Atoi(cond.value)
			op := cond.op
			if op == "" {
				op = "=="
			}
			ratingFilter = &ratingCond{op: op, value: v}
		case "albumrating":
			v, _ := strconv.Atoi(cond.value)
			op := cond.op
			if op == "" {
				op = "=="
			}
			albumRatingFilter = &ratingCond{op: op, value: v}
		default:
			filteredConditions = append(filteredConditions, cond)
		}
	}
	conditions = filteredConditions

	// Build a map of tag → value for quick lookup
	tags := map[string]string{}
	hasContains := false
	for _, cond := range conditions {
		tags[cond.tag] = cond.value
		if cond.op == "contains" {
			hasContains = true
		}
	}

	// Rating/albumrating-only query
	if (ratingFilter != nil || albumRatingFilter != nil) && len(conditions) == 0 {
		if albumRatingFilter != nil {
			albumIDs, err := a.db.albumIDsByRatingOp(albumRatingFilter.op, albumRatingFilter.value)
			if err != nil {
				return mpdErr(errSystem, cmdName, err.Error())
			}
			var allTracks []map[string]any
			for _, id := range albumIDs {
				tracks, err := a.db.tracksByAlbum(id)
				if err != nil {
					continue
				}
				allTracks = append(allTracks, tracks...)
			}
			return writeOrAddFilteredTracks(c, a, allTracks, ratingFilter, nil, cmdName, addToQueue)
		}
		// Track rating only — use indexed join instead of scanning all tracks
		tracks, err := a.db.tracksByRatingOp(ratingFilter.op, ratingFilter.value)
		if err != nil {
			return mpdErr(errSystem, cmdName, err.Error())
		}
		return writeOrAddFilteredTracks(c, a, tracks, nil, nil, cmdName, addToQueue)
	}

	// If we have "any contains X" → do text search
	if _, ok := tags["any"]; ok || hasContains {
		query := ""
		for _, cond := range conditions {
			if query != "" {
				query += " "
			}
			query += cond.value
		}
		return cmdTextSearch(c, query, ratingFilter, albumRatingFilter, cmdName, addToQueue)
	}

	// Try to resolve via structured DB queries
	artistFilter := tags["albumartist"]
	if artistFilter == "" {
		artistFilter = tags["artist"]
	}
	albumFilter := tags["album"]
	dateFilter := tags["date"]

	// If we have artist + album, look up the specific album and return its tracks
	if artistFilter != "" && albumFilter != "" {
		albums, err := a.db.albumsByArtist(artistFilter)
		if err != nil {
			return mpdErr(errSystem, cmdName, err.Error())
		}
		// Batch-fetch album ratings if needed
		var albumRatings map[string]int
		if albumRatingFilter != nil {
			hashes := make([]string, len(albums))
			for i, al := range albums {
				hashes[i] = albumRatingHash(stringify(al["albumartist"]), stringify(al["album"]), stringify(al["date"]))
			}
			albumRatings, _ = a.db.getRatingsBatch(hashes)
		}
		var allTracks []map[string]any
		for _, album := range albums {
			albumTitle := stringify(album["album"])
			albumDate := stringify(album["date"])

			match := false
			if caseInsensitive {
				match = strings.EqualFold(albumTitle, albumFilter)
			} else {
				match = albumTitle == albumFilter
			}
			if dateFilter != "" && albumDate != dateFilter {
				match = false
			}
			if !match {
				continue
			}
			if albumRatingFilter != nil {
				h := albumRatingHash(stringify(album["albumartist"]), albumTitle, albumDate)
				if !albumRatingFilter.matches(albumRatings[h]) {
					continue
				}
			}

			albumID, _ := strconv.ParseInt(stringify(album["id"]), 10, 64)
			tracks, err := a.db.tracksByAlbum(albumID)
			if err != nil {
				return mpdErr(errSystem, cmdName, err.Error())
			}
			allTracks = append(allTracks, tracks...)
		}
		return writeOrAddFilteredTracks(c, a, allTracks, ratingFilter, nil, cmdName, addToQueue)
	}

	// If we have just artist, return all tracks by that artist
	if artistFilter != "" {
		albums, err := a.db.albumsByArtist(artistFilter)
		if err != nil {
			return mpdErr(errSystem, cmdName, err.Error())
		}
		// Batch-fetch album ratings if needed
		var albumRatings map[string]int
		if albumRatingFilter != nil {
			hashes := make([]string, len(albums))
			for i, al := range albums {
				hashes[i] = albumRatingHash(stringify(al["albumartist"]), stringify(al["album"]), stringify(al["date"]))
			}
			albumRatings, _ = a.db.getRatingsBatch(hashes)
		}
		var allTracks []map[string]any
		for _, album := range albums {
			if albumRatingFilter != nil {
				h := albumRatingHash(stringify(album["albumartist"]), stringify(album["album"]), stringify(album["date"]))
				if !albumRatingFilter.matches(albumRatings[h]) {
					continue
				}
			}
			albumID, _ := strconv.ParseInt(stringify(album["id"]), 10, 64)
			tracks, err := a.db.tracksByAlbum(albumID)
			if err != nil {
				continue
			}
			allTracks = append(allTracks, tracks...)
		}
		return writeOrAddFilteredTracks(c, a, allTracks, ratingFilter, nil, cmdName, addToQueue)
	}

	// Fallback to text search with all filter values combined
	query := ""
	for _, cond := range conditions {
		if query != "" {
			query += " "
		}
		query += cond.value
	}
	return cmdTextSearch(c, query, ratingFilter, albumRatingFilter, cmdName, addToQueue)
}

// cmdTextSearch does a text-based search and returns or enqueues the results.
// writeOrAddFilteredTracks writes or enqueues tracks, optionally filtering by rating.
func writeOrAddFilteredTracks(c *mpdConn, a *app, tracks []map[string]any, ratingFilter, albumRatingFilter *ratingCond, cmdName string, addToQueue bool) *mpdError {
	// If album rating filter is active, batch-fetch album ratings and filter
	var albumRatings map[string]int
	if albumRatingFilter != nil {
		hashes := make([]string, len(tracks))
		for i, t := range tracks {
			hashes[i] = albumRatingHash(stringify(t["albumartist"]), stringify(t["album"]), stringify(t["date"]))
		}
		albumRatings, _ = a.db.getRatingsBatch(hashes)
	}
	var allSongIDs []string
	for _, track := range tracks {
		r := intFromAny(track["rating"], 0)
		if ratingFilter != nil && !ratingFilter.matches(r) {
			continue
		}
		if albumRatingFilter != nil {
			h := albumRatingHash(stringify(track["albumartist"]), stringify(track["album"]), stringify(track["date"]))
			if !albumRatingFilter.matches(albumRatings[h]) {
				continue
			}
		}
		if addToQueue {
			allSongIDs = append(allSongIDs, stringify(track["song_id"]))
		} else {
			c.writeTrack(track, -1, 0)
		}
	}
	if addToQueue && len(allSongIDs) > 0 {
		if err := a.addSongsToPlaylist(allSongIDs, "add"); err != nil {
			return mpdErr(errSystem, cmdName, err.Error())
		}
		a.mpdHub.notify(SubPlaylist)
	}
	return nil
}

func cmdTextSearch(c *mpdConn, query string, ratingFilter, albumRatingFilter *ratingCond, cmdName string, addToQueue bool) *mpdError {
	if query == "" && ratingFilter == nil && albumRatingFilter == nil {
		return nil
	}
	a := c.app

	var tracks []map[string]any
	if query != "" {
		_, tracks2, err := a.db.search(query, 1000)
		if err != nil {
			return mpdErr(errSystem, cmdName, err.Error())
		}
		tracks = tracks2
	} else {
		var err error
		tracks, err = a.db.allTracks()
		if err != nil {
			return mpdErr(errSystem, cmdName, err.Error())
		}
	}

	return writeOrAddFilteredTracks(c, a, tracks, ratingFilter, albumRatingFilter, cmdName, addToQueue)
}

// oldStyleToConditions converts old-style "tag value tag value" args to conditions.
func oldStyleToConditions(args []string) []filterCondition {
	if len(args) < 2 || len(args)%2 != 0 {
		return nil
	}
	var conditions []filterCondition
	for i := 0; i < len(args); i += 2 {
		conditions = append(conditions, filterCondition{
			tag:   strings.ToLower(args[i]),
			op:    "==",
			value: args[i+1],
		})
	}
	return conditions
}

func cmdFindAdd(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "findadd", "need filter arguments")
	}
	return cmdSearchOrFindInner(c, args, "findadd", false, true)
}

func cmdSearchAdd(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "searchadd", "need filter arguments")
	}
	return cmdSearchOrFindInner(c, args, "searchadd", true, true)
}

func cmdCount(c *mpdConn, args []string) *mpdError {
	trackCount, _ := c.app.db.trackCount()
	c.writeKV("songs", trackCount)
	c.writeKV("playtime", 0)
	return nil
}

func cmdListAll(c *mpdConn, args []string) *mpdError {
	a := c.app
	tracks, err := a.db.allTracks()
	if err != nil {
		return mpdErr(errSystem, "listall", err.Error())
	}
	for _, track := range tracks {
		path := stringify(track["path"])
		uri := c.pathToURI(path)
		c.writeKV("file", uri)
	}
	return nil
}

func cmdListAllInfo(c *mpdConn, args []string) *mpdError {
	a := c.app
	tracks, err := a.db.allTracks()
	if err != nil {
		return mpdErr(errSystem, "listallinfo", err.Error())
	}
	for _, track := range tracks {
		c.writeTrack(track, -1, 0)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Stored playlists
// ---------------------------------------------------------------------------

func cmdListPlaylists(c *mpdConn, args []string) *mpdError {
	playlists, err := c.app.db.allPlaylists()
	if err != nil {
		return mpdErr(errSystem, "listplaylists", err.Error())
	}
	for _, pl := range playlists {
		c.writeKV("playlist", stringify(pl["name"]))
		c.writeKV("Last-Modified", "2024-01-01T00:00:00Z")
	}
	return nil
}

func cmdListPlaylistInfo(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "listplaylistinfo", "need playlist name")
	}
	a := c.app
	name := args[0]

	playlists, err := a.db.allPlaylists()
	if err != nil {
		return mpdErr(errSystem, "listplaylistinfo", err.Error())
	}
	for _, pl := range playlists {
		if stringify(pl["name"]) == name {
			id, _ := strconv.ParseInt(stringify(pl["id"]), 10, 64)
			tracks, err := a.db.playlistTracks(id)
			if err != nil {
				return mpdErr(errSystem, "listplaylistinfo", err.Error())
			}
			for _, track := range tracks {
				c.writeTrack(track, -1, 0)
			}
			return nil
		}
	}
	return mpdErr(errNoExist, "listplaylistinfo", "playlist not found")
}

func cmdLoad(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "load", "need playlist name")
	}
	a := c.app
	name := args[0]

	playlists, err := a.db.allPlaylists()
	if err != nil {
		return mpdErr(errSystem, "load", err.Error())
	}
	for _, pl := range playlists {
		if stringify(pl["name"]) == name {
			id, _ := strconv.ParseInt(stringify(pl["id"]), 10, 64)
			songIDs, err := a.db.playlistTrackSongIDs(id)
			if err != nil {
				return mpdErr(errSystem, "load", err.Error())
			}
			if err := a.addSongsToPlaylist(songIDs, "add"); err != nil {
				return mpdErr(errSystem, "load", err.Error())
			}
			a.mpdHub.notify(SubPlaylist)
			return nil
		}
	}
	return mpdErr(errNoExist, "load", "playlist not found")
}

func cmdSave(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "save", "need playlist name")
	}
	a := c.app
	name := args[0]

	id, err := a.db.createPlaylist(name)
	if err != nil {
		return mpdErr(errSystem, "save", err.Error())
	}

	a.playQueueMu.Lock()
	queue := make([]string, len(a.playQueue))
	copy(queue, a.playQueue)
	a.playQueueMu.Unlock()

	for _, songID := range queue {
		trackID, _ := strconv.ParseInt(songID, 10, 64)
		_ = a.db.addTrackToPlaylist(id, trackID)
	}
	a.mpdHub.notify(SubStoredPlaylist)
	return nil
}

func cmdRm(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "rm", "need playlist name")
	}
	a := c.app
	name := args[0]

	playlists, err := a.db.allPlaylists()
	if err != nil {
		return mpdErr(errSystem, "rm", err.Error())
	}
	for _, pl := range playlists {
		if stringify(pl["name"]) == name {
			id, _ := strconv.ParseInt(stringify(pl["id"]), 10, 64)
			if err := a.db.deletePlaylist(id); err != nil {
				return mpdErr(errSystem, "rm", err.Error())
			}
			a.mpdHub.notify(SubStoredPlaylist)
			return nil
		}
	}
	return mpdErr(errNoExist, "rm", "playlist not found")
}

// cmdPlaylistAdd handles "playlistadd <name> <uri>" — adds a track to a stored playlist (creating it if needed).
func cmdPlaylistAdd(c *mpdConn, args []string) *mpdError {
	if len(args) < 2 {
		return mpdErr(errArg, "playlistadd", "need playlist name and URI")
	}
	a := c.app
	name := args[0]
	uri := args[1]

	playlistID, err := a.db.findOrCreatePlaylist(name)
	if err != nil {
		return mpdErr(errSystem, "playlistadd", err.Error())
	}

	trackID, err := a.db.trackIDByPath(uri)
	if err != nil {
		return mpdErr(errNoExist, "playlistadd", "track not found")
	}

	if err := a.db.addTrackToPlaylist(playlistID, trackID); err != nil {
		return mpdErr(errSystem, "playlistadd", err.Error())
	}
	a.mpdHub.notify(SubStoredPlaylist)
	return nil
}

// ---------------------------------------------------------------------------
// Output commands (device management)
// ---------------------------------------------------------------------------

func cmdOutputs(c *mpdConn, args []string) *mpdError {
	a := c.app
	a.devicesMu.RLock()
	defer a.devicesMu.RUnlock()

	devs := a.sortedDevices()
	for i, dev := range devs {
		enabled := 0
		if dev.ID == a.activeDevice {
			enabled = 1
		}
		c.writeKV("outputid", i)
		c.writeKV("outputname", dev.Name)
		c.writeKV("outputenabled", enabled)
		c.writeKV("plugin", dev.Type)
	}
	return nil
}

func cmdEnableOutput(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "enableoutput", "need output id")
	}
	idx, err := strconv.Atoi(args[0])
	if err != nil {
		return mpdErr(errArg, "enableoutput", "invalid output id")
	}

	a := c.app
	a.devicesMu.RLock()
	devs := a.sortedDevices()
	var target *device
	if idx >= 0 && idx < len(devs) {
		target = devs[idx]
	}
	a.devicesMu.RUnlock()

	if target == nil {
		return mpdErr(errNoExist, "enableoutput", "output not found")
	}

	// Full device handoff (capture state, transfer queue, resume)
	if err := a.switchDevice(target.ID); err != nil {
		return mpdErr(errSystem, "enableoutput", err.Error())
	}
	return nil
}

func cmdDisableOutput(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "disableoutput", "need output id")
	}
	// Disabling an output switches back to local with full handoff
	if err := c.app.switchDevice("local"); err != nil {
		return mpdErr(errSystem, "disableoutput", err.Error())
	}
	return nil
}

// ---------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------

func cmdSetVol(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "setvol", "need volume argument")
	}
	vol, err := strconv.Atoi(args[0])
	if err != nil {
		return mpdErr(errArg, "setvol", "invalid volume")
	}
	t := c.app.target()
	if err := t.setProperty("volume", vol); err != nil {
		return mpdErr(errSystem, "setvol", err.Error())
	}
	c.app.mpdHub.notify(SubMixer)
	return nil
}

func cmdVolume(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "volume", "need volume change argument")
	}
	change, err := strconv.Atoi(args[0])
	if err != nil {
		return mpdErr(errArg, "volume", "invalid volume change")
	}
	t := c.app.target()
	volRaw, _ := t.getProperty("volume")
	vol, _ := volRaw.(float64)
	newVol := int(vol) + change
	if newVol < 0 {
		newVol = 0
	}
	if newVol > 100 {
		newVol = 100
	}
	if err := t.setProperty("volume", newVol); err != nil {
		return mpdErr(errSystem, "volume", err.Error())
	}
	c.app.mpdHub.notify(SubMixer)
	return nil
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

func cmdReplayGainMode(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "replay_gain_mode", "need mode argument")
	}
	c.app.cfg.MPV.ReplayGain = args[0]
	c.app.mpdHub.notify(SubOptions)
	return nil
}

func cmdReplayGainStatus(c *mpdConn, args []string) *mpdError {
	mode := c.app.cfg.MPV.ReplayGain
	if mode == "" {
		mode = "off"
	}
	c.writeKV("replay_gain_mode", mode)
	return nil
}

func cmdIgnore(c *mpdConn, args []string) *mpdError {
	return nil
}

// ---------------------------------------------------------------------------
// Connection commands
// ---------------------------------------------------------------------------

func cmdPing(c *mpdConn, args []string) *mpdError {
	return nil
}

func cmdChannels(c *mpdConn, args []string) *mpdError {
	return nil // empty list, OK
}

func cmdEmpty(c *mpdConn, args []string) *mpdError {
	return nil // empty response, OK
}

func cmdCommands(c *mpdConn, args []string) *mpdError {
	for name := range commandTable {
		c.writeKV("command", name)
	}
	// Also list idle and close which are handled specially
	c.writeKV("command", "idle")
	c.writeKV("command", "noidle")
	c.writeKV("command", "close")
	return nil
}

func cmdNotCommands(c *mpdConn, args []string) *mpdError {
	return nil
}

func cmdTagTypes(c *mpdConn, args []string) *mpdError {
	for _, t := range []string{"Artist", "AlbumArtist", "Album", "Title", "Track", "Date", "Disc"} {
		c.writeKV("tagtype", t)
	}
	return nil
}

func cmdDecoders(c *mpdConn, args []string) *mpdError {
	return nil
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// writeTrack writes a track in MPD response format.
func (c *mpdConn) writeTrack(track map[string]any, pos int, mpdID int) {
	path := stringify(track["path"])
	if path == "" {
		// Build path from metadata if not available
		path = stringify(track["albumartist"]) + "/" + stringify(track["album"]) + "/" + stringify(track["title"])
	}
	uri := c.pathToURI(path)
	c.writeKV("file", uri)
	if v := stringify(track["artist"]); v != "" {
		c.writeKV("Artist", v)
	}
	if v := stringify(track["albumartist"]); v != "" {
		c.writeKV("AlbumArtist", v)
	}
	if v := stringify(track["title"]); v != "" {
		c.writeKV("Title", v)
	}
	if v := stringify(track["album"]); v != "" {
		c.writeKV("Album", v)
	}
	if v := stringify(track["date"]); v != "" && v != "0000" {
		c.writeKV("Date", v)
	}
	if v := intFromAny(track["tracknumber"], 0); v > 0 {
		c.writeKV("Track", v)
	}
	if v := intFromAny(track["discnumber"], 0); v > 0 {
		c.writeKV("Disc", v)
	}
	dur := 0.0
	if d, ok := track["duration"].(float64); ok {
		dur = d
	}
	if dur > 0 {
		c.writeKV("Time", int(math.Ceil(dur)))
		c.writef("duration: %.3f\n", dur)
	}
	if pos >= 0 {
		c.writeKV("Pos", pos)
	}
	if mpdID > 0 {
		c.writeKV("Id", mpdID)
	}
	// Custom extensions for melody clients
	if v := stringify(track["song_id"]); v != "" {
		c.writeKV("X-SongId", v)
	}
	if v := stringify(track["album_id"]); v != "" {
		c.writeKV("X-AlbumId", v)
	}
	if v := intFromAny(track["rating"], 0); v > 0 {
		c.writeKV("X-Rating", v)
	}
}

func (c *mpdConn) pathToURI(path string) string {
	rel, err := filepath.Rel(c.app.cfg.Library.MusicDir, path)
	if err != nil {
		return path
	}
	return rel
}

// parseRange parses MPD range arguments: "3" → (3,4), "2:5" → (2,5)
func parseRange(s string, queueLen int) (int, int, error) {
	if strings.Contains(s, ":") {
		parts := strings.SplitN(s, ":", 2)
		start, err := strconv.Atoi(parts[0])
		if err != nil {
			return 0, 0, fmt.Errorf("invalid range start")
		}
		end, err := strconv.Atoi(parts[1])
		if err != nil {
			return 0, 0, fmt.Errorf("invalid range end")
		}
		if start < 0 || end > queueLen || start > end {
			return 0, 0, fmt.Errorf("range out of bounds")
		}
		return start, end, nil
	}
	pos, err := strconv.Atoi(s)
	if err != nil {
		return 0, 0, fmt.Errorf("invalid position")
	}
	if pos < 0 || pos >= queueLen {
		return 0, 0, fmt.Errorf("position out of bounds")
	}
	return pos, pos + 1, nil
}

// filterCondition represents a single tag == value or tag contains value clause.
type filterCondition struct {
	tag   string // e.g. "albumartist", "album", "any"
	op    string // "==" or "contains"
	value string
}

// parseFilterExpr parses MPD new-style filter expressions like:
//
//	"((AlbumArtist == \"foo\") AND (Album == \"bar\"))"
//	"(any contains \"query\")"
//	"(AlbumArtist == \"foo\")"
//
// Returns a slice of conditions ANDed together.
func parseFilterExpr(expr string) []filterCondition {
	// Strip outer parens layers
	expr = strings.TrimSpace(expr)
	var conditions []filterCondition

	// Split on " AND " (case-insensitive would be nice but MPD uses uppercase)
	// First strip the outermost parens if present
	for strings.HasPrefix(expr, "(") && strings.HasSuffix(expr, ")") {
		inner := expr[1 : len(expr)-1]
		// Check if removing outer parens is balanced
		depth := 0
		balanced := true
		for _, r := range inner {
			if r == '(' {
				depth++
			} else if r == ')' {
				depth--
			}
			if depth < 0 {
				balanced = false
				break
			}
		}
		if balanced && depth == 0 {
			expr = inner
		} else {
			break
		}
	}

	// Split by AND
	parts := splitFilterAND(expr)
	for _, part := range parts {
		part = strings.TrimSpace(part)
		// Strip parens from individual clause
		for strings.HasPrefix(part, "(") && strings.HasSuffix(part, ")") {
			part = part[1 : len(part)-1]
		}
		cond := parseOneCondition(part)
		if cond.tag != "" {
			conditions = append(conditions, cond)
		}
	}
	return conditions
}

// splitFilterAND splits an expression on " AND " respecting parenthesis depth.
func splitFilterAND(s string) []string {
	var parts []string
	depth := 0
	start := 0
	for i := 0; i < len(s); i++ {
		switch s[i] {
		case '(':
			depth++
		case ')':
			depth--
		}
		if depth == 0 && i+5 <= len(s) && s[i:i+5] == " AND " {
			parts = append(parts, s[start:i])
			start = i + 5
			i += 4
		}
	}
	parts = append(parts, s[start:])
	return parts
}

// parseOneCondition parses "Tag == \"value\"" or "Tag contains \"value\"".
func parseOneCondition(s string) filterCondition {
	s = strings.TrimSpace(s)
	for _, op := range []string{" >= ", " <= ", " == ", " > ", " < ", " contains "} {
		idx := strings.Index(s, op)
		if idx < 0 {
			continue
		}
		tag := strings.TrimSpace(s[:idx])
		val := strings.TrimSpace(s[idx+len(op):])
		// Strip quotes from value
		val = stripQuotes(val)
		return filterCondition{
			tag:   strings.ToLower(tag),
			op:    strings.TrimSpace(op),
			value: val,
		}
	}
	return filterCondition{}
}

func stripQuotes(s string) string {
	if len(s) >= 2 {
		if (s[0] == '"' && s[len(s)-1] == '"') || (s[0] == '\'' && s[len(s)-1] == '\'') {
			return s[1 : len(s)-1]
		}
	}
	return s
}

// ---------------------------------------------------------------------------
// Cover art commands
// ---------------------------------------------------------------------------
// Rating commands (custom extension)
// ---------------------------------------------------------------------------

// cmdRate handles "rate <songid> <rating>" — rate a track (0-10, 0=unrate).
func cmdRate(c *mpdConn, args []string) *mpdError {
	if len(args) < 2 {
		return mpdErr(errArg, "rate", "need songid and rating")
	}
	track, err := c.app.db.trackBySongID(args[0])
	if err != nil {
		return mpdErr(errNoExist, "rate", "unknown song id")
	}
	rating, err := strconv.Atoi(args[1])
	if err != nil || rating < 0 || rating > 10 {
		return mpdErr(errArg, "rate", "rating must be 0-10")
	}
	hash := stringify(track["rating_hash"])
	if hash == "" {
		return mpdErr(errSystem, "rate", "cannot compute rating hash")
	}
	if err := c.app.db.setRating(hash, "track", rating); err != nil {
		return mpdErr(errSystem, "rate", err.Error())
	}
	c.app.mpdHub.notify(SubRating)
	return nil
}

// cmdAlbumRate handles "albumrate <albumartist> <album> <date> <rating>".
func cmdAlbumRate(c *mpdConn, args []string) *mpdError {
	if len(args) < 4 {
		return mpdErr(errArg, "albumrate", "need albumartist, album, date, rating")
	}
	rating, err := strconv.Atoi(args[3])
	if err != nil || rating < 0 || rating > 10 {
		return mpdErr(errArg, "albumrate", "rating must be 0-10")
	}
	hash := albumRatingHash(args[0], args[1], args[2])
	if err := c.app.db.setRating(hash, "album", rating); err != nil {
		return mpdErr(errSystem, "albumrate", err.Error())
	}
	c.app.mpdHub.notify(SubRating)
	return nil
}

// cmdGetRating handles "getrating <songid>".
func cmdGetRating(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "getrating", "need songid")
	}
	track, err := c.app.db.trackBySongID(args[0])
	if err != nil {
		return mpdErr(errNoExist, "getrating", "unknown song id")
	}
	r := intFromAny(track["rating"], 0)
	c.writeKV("rating", r)
	return nil
}

// cmdGetAlbumRating handles "getalbumrating <albumartist> <album> <date>".
// Returns both the user-set album rating and the computed average of track ratings.
func cmdGetAlbumRating(c *mpdConn, args []string) *mpdError {
	if len(args) < 3 {
		return mpdErr(errArg, "getalbumrating", "need albumartist, album, date")
	}
	albumArtist, album, date := args[0], args[1], args[2]

	// User-set album rating
	hash := albumRatingHash(albumArtist, album, date)
	userRating, _ := c.app.db.getRating(hash)
	c.writeKV("rating", userRating)

	// Computed average from track ratings
	albums, err := c.app.db.albumsByArtist(albumArtist)
	if err == nil {
		for _, a := range albums {
			if stringify(a["album"]) == album && stringify(a["date"]) == date {
				albumID, _ := strconv.ParseInt(stringify(a["id"]), 10, 64)
				tracks, err := c.app.db.tracksByAlbum(albumID)
				if err == nil {
					sum, count := 0, 0
					for _, t := range tracks {
						if r := intFromAny(t["rating"], 0); r > 0 {
							sum += r
							count++
						}
					}
					if count > 0 {
						c.writef("computed: %.1f\n", float64(sum)/float64(count))
					} else {
						c.writeKV("computed", "0.0")
					}
				}
				break
			}
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// Cover art
// ---------------------------------------------------------------------------

const defaultBinaryLimit = 65536 // 64KB default chunk size

// cmdAlbumArt handles "albumart <uri> <offset>" — returns cover art for the
// directory containing the given URI. Tries embedded art first, then folder art.
func cmdAlbumArt(c *mpdConn, args []string) *mpdError {
	if len(args) < 2 {
		return mpdErr(errArg, "albumart", "need URI and offset arguments")
	}
	uri := args[0]
	offset, err := strconv.Atoi(args[1])
	if err != nil || offset < 0 {
		return mpdErr(errArg, "albumart", "invalid offset")
	}

	absPath := filepath.Join(c.app.cfg.Library.MusicDir, uri)
	data, mimeType := getCoverArt(absPath)
	if data == nil {
		return mpdErr(errNoExist, "albumart", "no cover art")
	}

	return writeBinaryResponse(c, data, mimeType, offset)
}

// cmdReadPicture handles "readpicture <uri> <offset>" — same as albumart but
// per the MPD spec it reads embedded art from the specific file.
func cmdReadPicture(c *mpdConn, args []string) *mpdError {
	if len(args) < 2 {
		return mpdErr(errArg, "readpicture", "need URI and offset arguments")
	}
	uri := args[0]
	offset, err := strconv.Atoi(args[1])
	if err != nil || offset < 0 {
		return mpdErr(errArg, "readpicture", "invalid offset")
	}

	absPath := filepath.Join(c.app.cfg.Library.MusicDir, uri)
	data, mimeType := extractCoverArt(absPath)
	if data == nil {
		// readpicture returns empty OK (size: 0) when no embedded picture
		c.writeKV("size", 0)
		return nil
	}

	return writeBinaryResponse(c, data, mimeType, offset)
}

// getCoverArt returns cover art for a track path: tries embedded first, then folder.
func getCoverArt(trackPath string) ([]byte, string) {
	data, mimeType := extractCoverArt(trackPath)
	if data != nil {
		return data, mimeType
	}
	artPath := findFolderArt(filepath.Dir(trackPath))
	if artPath == "" {
		return nil, ""
	}
	fileData, err := os.ReadFile(artPath)
	if err != nil {
		return nil, ""
	}
	ext := strings.ToLower(filepath.Ext(artPath))
	switch ext {
	case ".png":
		mimeType = "image/png"
	default:
		mimeType = "image/jpeg"
	}
	return fileData, mimeType
}

// writeBinaryResponse writes an MPD binary response chunk.
func writeBinaryResponse(c *mpdConn, data []byte, mimeType string, offset int) *mpdError {
	total := len(data)
	if offset >= total {
		return mpdErr(errArg, "", "offset beyond end of data")
	}

	chunk := data[offset:]
	chunkSize := len(chunk)
	if chunkSize > defaultBinaryLimit {
		chunkSize = defaultBinaryLimit
	}
	chunk = chunk[:chunkSize]

	c.writeKV("size", total)
	if mimeType != "" {
		c.writeKV("type", mimeType)
	}
	c.writeKV("binary", chunkSize)
	c.writer.Write(chunk)
	c.writef("\n")
	return nil
}
