// SPDX-License-Identifier: GPL-3.0-only
package main

import (
	"encoding/json"
	"fmt"
	"github.com/carnager/melody/internal/lastfm"
	"path/filepath"
	"strconv"
	"time"
)

func (a *app) startLastFM() {
	a.lastfm = lastfm.New(filepath.Join(filepath.Dir(a.paths.PlayQueueFile), "lastfm-v1.json"))
	go func() {
		ticker := time.NewTicker(time.Second)
		defer ticker.Stop()
		for now := range ticker.C {
			a.sampleLastFM(now)
		}
	}()
	go func() {
		ticker := time.NewTicker(time.Second)
		defer ticker.Stop()
		for now := range ticker.C {
			a.lastfm.Flush(now)
		}
	}()
}
func (a *app) sampleLastFM(now time.Time) {
	a.playQueueMu.Lock()
	id := ""
	var dbid string
	if a.curQueuePos >= 0 && a.curQueuePos < len(a.queueIDs) {
		id = fmt.Sprintf("%d/%d", a.queueIDs[a.curQueuePos], a.scrobbleGeneration)
		dbid = a.playQueue[a.curQueuePos]
	}
	a.playQueueMu.Unlock()
	t := a.target()
	playing := false
	position := 0.0
	if t.isRunning() && !targetStopped(t) {
		p, err := t.getProperty("pause")
		paused, ok := p.(bool)
		playing = err == nil && ok && !paused
		if v, e := t.getProperty("time-pos"); e == nil {
			position, _ = v.(float64)
		} else {
			playing = false
		}
	} else {
		id = ""
	}
	var track lastfm.Track
	if id != "" {
		n, _ := strconv.ParseInt(dbid, 10, 64)
		if row, err := a.db.trackByID(n); err == nil {
			track.Artist, _ = row["artist"].(string)
			track.Title, _ = row["title"].(string)
			track.Album, _ = row["album"].(string)
			track.Duration, _ = row["duration"].(float64)
		}
	}
	a.lastfm.Observe(id, track, position, playing, now)
}
func cmdMelodyLastFM(c *mpdConn, args []string) *mpdError {
	if c.app.lastfm == nil {
		return mpdErr(errSystem, "melody_lastfm", "Last.fm service unavailable")
	}
	if len(args) == 0 {
		args = []string{"status"}
	}
	state, err := c.app.lastfm.Execute(args[0], args[1:])
	if err != nil {
		return mpdErr(errSystem, "melody_lastfm", err.Error())
	}
	b, err := json.Marshal(state)
	if err != nil {
		return mpdErr(errSystem, "melody_lastfm", "invalid Last.fm state")
	}
	c.writeKV("lastfm", string(b))
	return nil
}
