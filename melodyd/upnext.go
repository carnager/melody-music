// SPDX-License-Identifier: GPL-3.0-only
package main

import (
	"slices"
	"strconv"
)

// requestQueue contains occurrence IDs, never library IDs. The projected MPD
// queue includes these occurrences, but normal traversal excludes them.
// All methods below run under playQueueMu unless documented otherwise.
type requestQueue struct {
	Pending []int `json:"pending,omitempty"`
	Active  int   `json:"active,omitempty"`
	Return  []int `json:"return,omitempty"`
}

type requestUndoState struct {
	Revision int
	IDs      []int
	Songs    []string
}

func (a *app) rememberRequestEdit() *requestUndoState {
	state := &requestUndoState{IDs: slices.Clone(a.upNext.Pending)}
	for _, id := range state.IDs {
		state.Songs = append(state.Songs, a.playQueue[a.requestPosition(id)])
	}
	return state
}

func (a *app) requestPosition(id int) int { return slices.Index(a.queueIDs, id) }
func (a *app) isRequest(id int) bool {
	return id != 0 && (id == a.upNext.Active || slices.Contains(a.upNext.Pending, id))
}

func (a *app) reconcileRequests() {
	a.upNext.Pending = slices.DeleteFunc(a.upNext.Pending, func(id int) bool { return a.requestPosition(id) < 0 })
	a.upNext.Return = slices.DeleteFunc(a.upNext.Return, func(id int) bool { return a.requestPosition(id) < 0 })
}

// normalNextPosition evaluates the existing traversal against the base list,
// excluding temporary occurrences. Shuffle indices are projected in both
// directions, keeping the existing random traversal and priority semantics.
func (a *app) normalNextPosition() int {
	songs, ids, priorities := a.playQueue, a.queueIDs, a.queuePriority
	current, order, cursor, ret := a.curQueuePos, a.shuffleOrder, a.shufflePos, a.prioReturnPos
	var positions []int
	a.playQueue, a.queueIDs, a.queuePriority = nil, nil, nil
	a.curQueuePos, a.prioReturnPos = -1, -1
	for i, id := range ids {
		if a.isRequest(id) {
			continue
		}
		if i == current {
			a.curQueuePos = len(positions)
		}
		if i == ret {
			a.prioReturnPos = len(positions)
		}
		positions = append(positions, i)
		a.playQueue = append(a.playQueue, songs[i])
		a.queueIDs = append(a.queueIDs, id)
		p := 0
		if i < len(priorities) {
			p = priorities[i]
		}
		a.queuePriority = append(a.queuePriority, p)
	}
	a.shuffleOrder = nil
	a.shufflePos = -1
	for i, pos := range order {
		if mapped := slices.Index(positions, pos); mapped >= 0 {
			a.shuffleOrder = append(a.shuffleOrder, mapped)
			if i <= cursor {
				a.shufflePos = len(a.shuffleOrder) - 1
			}
		}
	}
	next := a.baseNextQueuePos()
	updatedOrder := make([]int, 0, len(a.shuffleOrder))
	for _, pos := range a.shuffleOrder {
		if pos >= 0 && pos < len(positions) {
			updatedOrder = append(updatedOrder, positions[pos])
		}
	}
	updatedCursor := a.shufflePos
	a.playQueue, a.queueIDs, a.queuePriority = songs, ids, priorities
	a.curQueuePos, a.prioReturnPos = current, ret
	a.shuffleOrder, a.shufflePos = updatedOrder, updatedCursor
	if next >= 0 && next < len(positions) {
		return positions[next]
	}
	return -1
}

func (a *app) nextQueuePos() int {
	if len(a.upNext.Pending) == 0 && a.upNext.Active == 0 {
		return a.baseNextQueuePos()
	}
	a.reconcileRequests()
	if a.modeSingle {
		return -1
	}
	if len(a.upNext.Pending) > 0 {
		return a.requestPosition(a.upNext.Pending[0])
	}
	for _, id := range a.upNext.Return {
		if pos := a.requestPosition(id); pos >= 0 {
			return pos
		}
	}
	return -1
}

func (a *app) captureRequestReturn() {
	if a.upNext.Active != 0 {
		return
	}
	// Single controls natural advancement, but an explicit Next still captures
	// the normal successor rather than returning to the completed song.
	single := a.modeSingle
	a.modeSingle = false
	next := a.normalNextPosition()
	a.modeSingle = single
	a.upNext.Return = nil
	if next < 0 {
		return
	}
	a.upNext.Return = append(a.upNext.Return, a.queueIDs[next])
	// Stable fallbacks follow the captured shuffle traversal as well.
	if a.modeRandom {
		if index := slices.Index(a.shuffleOrder, next); index >= 0 {
			for _, pos := range a.shuffleOrder[index+1:] {
				if pos >= 0 && pos < len(a.queueIDs) && !a.isRequest(a.queueIDs[pos]) {
					a.upNext.Return = append(a.upNext.Return, a.queueIDs[pos])
				}
			}
		}
		return
	}
	for i := next + 1; i < len(a.queueIDs); i++ {
		if !a.isRequest(a.queueIDs[i]) {
			a.upNext.Return = append(a.upNext.Return, a.queueIDs[i])
		}
	}
}

func (a *app) eraseRequest(id int) {
	pos := a.requestPosition(id)
	if pos < 0 {
		return
	}
	a.playQueue = append(a.playQueue[:pos], a.playQueue[pos+1:]...)
	a.queueIDs = append(a.queueIDs[:pos], a.queueIDs[pos+1:]...)
	a.queuePriority = append(a.queuePriority[:pos], a.queuePriority[pos+1:]...)
	if a.curQueuePos > pos {
		a.curQueuePos--
	} else if a.curQueuePos == pos {
		a.curQueuePos = -1
	}
	if a.pendingNextPos > pos {
		a.pendingNextPos--
	} else if a.pendingNextPos == pos {
		a.pendingNextPos = -1
	}
	var order []int
	cursor := -1
	for i, p := range a.shuffleOrder {
		if p == pos {
			continue
		}
		if p > pos {
			p--
		}
		order = append(order, p)
		if i <= a.shufflePos {
			cursor = len(order) - 1
		}
	}
	a.shuffleOrder, a.shufflePos = order, cursor
}

// advanceRequest commits a selected/preloaded occurrence and returns whether
// it handled the boundary. Natural transitions must use the actual preload,
// not a potentially newer queue head.
func (a *app) advanceRequest(next int) bool {
	a.reconcileRequests()
	nextID := 0
	if next >= 0 && next < len(a.queueIDs) {
		nextID = a.queueIDs[next]
	}
	if a.upNext.Active == 0 && !slices.Contains(a.upNext.Pending, nextID) {
		return false
	}
	if a.upNext.Active == 0 {
		a.captureRequestReturn()
		if a.modeConsume && a.curQueuePos >= 0 && a.curQueuePos < len(a.queueIDs) {
			a.eraseRequest(a.queueIDs[a.curQueuePos])
		}
	} else {
		a.eraseRequest(a.upNext.Active)
	}
	if slices.Contains(a.upNext.Pending, nextID) {
		a.upNext.Pending = slices.DeleteFunc(a.upNext.Pending, func(id int) bool { return id == nextID })
		a.upNext.Active = nextID
	} else {
		a.upNext.Active = 0
		a.upNext.Return = nil
	}
	a.curQueuePos = a.requestPosition(nextID)
	if a.upNext.Active == 0 && a.modeRandom {
		a.shufflePos = slices.Index(a.shuffleOrder, a.curQueuePos)
	}
	a.bumpQueueVersionLocked()
	a.savePlayQueue()
	return true
}

func (a *app) clearRequests() {
	for _, id := range a.upNext.Pending {
		a.eraseRequest(id)
	}
	a.upNext.Pending = nil
}

func (a *app) abandonRequests() {
	returnID := 0
	for _, id := range a.upNext.Return {
		if a.requestPosition(id) >= 0 {
			returnID = id
			break
		}
	}
	a.clearRequests()
	if a.upNext.Active != 0 {
		a.eraseRequest(a.upNext.Active)
		a.curQueuePos = a.requestPosition(returnID)
	}
	a.upNext = requestQueue{}
}

// cmdMelodyUpNextEdit atomically retains/reorders pending occurrence IDs.
// Its separately advertised capability lets clients detect batch-edit support.
func cmdMelodyUpNextEdit(c *mpdConn, args []string) *mpdError {
	return cmdMelodyUpNext(c, append([]string{"retain"}, args...))
}

func cmdMelodyUpNext(c *mpdConn, args []string) *mpdError {
	a := c.app
	if len(args) == 0 {
		a.playQueueMu.Lock()
		a.reconcileRequests()
		c.writeKV("revision", a.queueVersion)
		c.writeKV("active", a.upNext.Active)
		canUndo := 0
		if a.requestUndo != nil && a.requestUndo.Revision == a.queueVersion {
			canUndo = 1
		}
		c.writeKV("undo", canUndo)
		c.writeKV("context", a.activeContext)
		ids := slices.Clone(a.upNext.Pending)
		songs := make([]string, 0, len(ids))
		for _, id := range ids {
			songs = append(songs, a.playQueue[a.requestPosition(id)])
		}
		a.playQueueMu.Unlock()
		for i, song := range songs {
			if track := a.findTrackBySongID(song); track != nil {
				c.writeTrack(track, i, ids[i])
			}
		}
		return nil
	}
	if len(args) < 2 {
		return mpdErr(errArg, "melody_upnext", "expected operation and revision")
	}
	revision, err := strconv.Atoi(args[1])
	if err != nil {
		return mpdErr(errArg, "melody_upnext", "bad revision")
	}
	var songs []string
	if args[0] == "append" || args[0] == "prepend" || args[0] == "insert" {
		var e *mpdError
		uris := args[2:]
		if args[0] == "insert" {
			if len(args) < 3 {
				return mpdErr(errArg, "melody_upnext", "expected insertion position")
			}
			uris = args[3:]
		}
		songs, e = c.contextListArgument(uris)
		if e != nil {
			return e
		}
		if len(songs) > 500 {
			return mpdErr(errArg, "melody_upnext", "at most 500 requests per batch")
		}
	}
	a.playQueueMu.Lock()
	if revision != a.queueVersion {
		a.playQueueMu.Unlock()
		return mpdErr(errArg, "melody_upnext", "request queue changed; refresh before retrying")
	}
	a.reconcileRequests()
	if args[0] == "return" {
		a.clearRequests()
		if a.upNext.Active != 0 {
			single := a.modeSingle
			a.modeSingle = false
			next := a.nextQueuePos()
			a.modeSingle = single
			a.advanceRequest(next)
		}
		a.bumpQueueVersionLocked()
		a.savePlayQueue()
		plan := a.planSyncTarget()
		a.playQueueMu.Unlock()
		a.execSyncPlan(plan)
		a.mpdHub.notify(SubPlaylist, SubPlayer)
		return nil
	}
	previous := a.rememberRequestEdit()
	switch args[0] {
	case "retain":
		if len(args)-2 > 500 {
			a.playQueueMu.Unlock()
			return mpdErr(errArg, "melody_upnext_edit", "at most 500 pending IDs")
		}
		ids := make([]int, 0, len(args)-2)
		seen := make(map[int]bool)
		for _, raw := range args[2:] {
			id, err := strconv.Atoi(raw)
			if err != nil || seen[id] || !slices.Contains(a.upNext.Pending, id) {
				a.playQueueMu.Unlock()
				return mpdErr(errArg, "melody_upnext_edit", "IDs must name distinct pending requests")
			}
			seen[id] = true
			ids = append(ids, id)
		}
		for _, id := range a.upNext.Pending {
			if !seen[id] {
				a.eraseRequest(id)
			}
		}
		a.upNext.Pending = ids
	case "undo":
		if a.requestUndo == nil || a.requestUndo.Revision != a.queueVersion {
			a.playQueueMu.Unlock()
			return mpdErr(errArg, "melody_upnext", "nothing to undo at this revision")
		}
		saved := a.requestUndo
		a.clearRequests()
		for i, id := range saved.IDs {
			a.playQueue = append(a.playQueue, saved.Songs[i])
			a.queueIDs = append(a.queueIDs, id)
			a.queuePriority = append(a.queuePriority, 0)
		}
		a.upNext.Pending = saved.IDs
		previous = nil

	case "append", "prepend", "insert":
		position := len(a.upNext.Pending)
		if args[0] == "prepend" {
			position = 0
		}
		if args[0] == "insert" {
			var e error
			position, e = strconv.Atoi(args[2])
			if e != nil || position < 0 || position > len(a.upNext.Pending) {
				a.playQueueMu.Unlock()
				return mpdErr(errArg, "melody_upnext", "bad insertion position")
			}
		}
		if len(a.upNext.Pending)+len(songs) > 500 {
			a.playQueueMu.Unlock()
			return mpdErr(errArg, "melody_upnext", "request queue limit is 500")
		}
		var ids []int
		for _, song := range songs {
			a.queueIDCounter++
			id := a.queueIDCounter
			a.playQueue = append(a.playQueue, song)
			a.queueIDs = append(a.queueIDs, id)
			a.queuePriority = append(a.queuePriority, 0)
			ids = append(ids, id)
		}
		a.upNext.Pending = slices.Insert(a.upNext.Pending, position, ids...)
	case "clear":
		a.clearRequests()
	case "remove", "move", "play":
		if len(args) < 3 {
			a.playQueueMu.Unlock()
			return mpdErr(errArg, "melody_upnext", "expected request ID")
		}
		id, e := strconv.Atoi(args[2])
		from := slices.Index(a.upNext.Pending, id)
		if e != nil || from < 0 {
			a.playQueueMu.Unlock()
			return mpdErr(errNoExist, "melody_upnext", "request no longer pending")
		}
		if args[0] == "play" {
			a.advanceRequest(a.requestPosition(id))
			a.savePlayQueue()
			plan := a.planSyncTarget()
			a.playQueueMu.Unlock()
			a.execSyncPlan(plan)
			a.mpdHub.notify(SubPlaylist, SubPlayer)
			return nil
		}
		if args[0] == "remove" {
			a.upNext.Pending = slices.Delete(a.upNext.Pending, from, from+1)
			a.eraseRequest(id)
		} else {
			if len(args) != 4 {
				a.playQueueMu.Unlock()
				return mpdErr(errArg, "melody_upnext", "expected target position")
			}
			to, e := strconv.Atoi(args[3])
			if e != nil || to < 0 || to >= len(a.upNext.Pending) {
				a.playQueueMu.Unlock()
				return mpdErr(errArg, "melody_upnext", "bad target position")
			}
			a.upNext.Pending = slices.Delete(a.upNext.Pending, from, from+1)
			a.upNext.Pending = slices.Insert(a.upNext.Pending, to, id)
		}
	default:
		a.playQueueMu.Unlock()
		return mpdErr(errArg, "melody_upnext", "unknown operation")
	}
	a.bumpQueueVersionLocked()
	a.requestUndo = previous
	if previous != nil {
		previous.Revision = a.queueVersion
	}
	saveErr := a.savePlayQueue()
	plan := a.planNextTrack()
	a.playQueueMu.Unlock()
	a.execNextTrackPlan(plan)
	a.mpdHub.notify(SubPlaylist, SubPlayer)
	if saveErr != nil {
		return mpdErr(errSystem, "melody_upnext", "queue changed but could not be saved: "+saveErr.Error())
	}
	return nil
}

// refreshRequestBase preserves FIFO identities while a stored playlist is edited.
func (a *app) refreshRequestBase(songs []string) {
	currentID := 0
	if a.curQueuePos >= 0 && a.curQueuePos < len(a.queueIDs) {
		currentID = a.queueIDs[a.curQueuePos]
	}
	type occurrence struct{ id, priority int }
	available := map[string][]occurrence{}
	requests := map[int]string{}
	for i, id := range a.queueIDs {
		if a.isRequest(id) {
			requests[id] = a.playQueue[i]
			continue
		}
		p := 0
		if i < len(a.queuePriority) {
			p = a.queuePriority[i]
		}
		available[a.playQueue[i]] = append(available[a.playQueue[i]], occurrence{id, p})
	}
	a.playQueue = nil
	a.queueIDs = nil
	a.queuePriority = nil
	for _, song := range songs {
		item := occurrence{}
		if len(available[song]) > 0 {
			item = available[song][0]
			available[song] = available[song][1:]
		} else {
			a.queueIDCounter++
			item.id = a.queueIDCounter
		}
		a.playQueue = append(a.playQueue, song)
		a.queueIDs = append(a.queueIDs, item.id)
		a.queuePriority = append(a.queuePriority, item.priority)
	}
	ids := append([]int{}, a.upNext.Pending...)
	if a.upNext.Active != 0 {
		ids = append(ids, a.upNext.Active)
	}
	for _, id := range ids {
		if song, ok := requests[id]; ok {
			a.playQueue = append(a.playQueue, song)
			a.queueIDs = append(a.queueIDs, id)
			a.queuePriority = append(a.queuePriority, 0)
		}
	}
	a.curQueuePos = a.requestPosition(currentID)
	a.pendingNextPos = -1
	a.reconcileRequests()
	if a.modeRandom {
		a.generateShuffle()
	}
	a.bumpQueueVersionLocked()
}
