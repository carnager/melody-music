package main

import (
	"sort"
	"strconv"
	"strings"
)

// searchalbums implements the album-shaped search from docs/protocol.md:
// the same filter expressions as search, evaluated against albums, with
// optional sort and window, answering one record per album instead of the
// protocol's usual song list.
//
// Album-level terms (albumartist, album, date, albumrating, added-since)
// apply to the album record directly; every other supported term matches
// albums containing at least one matching track. Unsupported terms are an
// ACK, never a silent broadening of the result.

// albumSortKeys lists the tags searchalbums accepts in "sort [-]TAG".
var albumSortKeys = map[string]bool{
	"albumartist": true,
	"album":       true,
	"date":        true,
	"added":       true,
	"rating":      true,
}

type albumSearchTerms struct {
	albumArtist  *filterCondition
	album        *filterCondition
	date         *filterCondition
	albumRating  *ratingCond
	addedSince   int64
	trackLevel   []filterCondition
	trackRating  *ratingCond
	technical    []filterCondition
	textWords    []string
	unsupportedT string
}

func splitAlbumSearchTerms(conditions []filterCondition) albumSearchTerms {
	var terms albumSearchTerms
	for i := range conditions {
		cond := conditions[i]
		switch cond.tag {
		case "albumartist":
			terms.albumArtist = &conditions[i]
		case "album":
			terms.album = &conditions[i]
		case "date":
			terms.date = &conditions[i]
		case "albumrating":
			v, _ := strconv.Atoi(cond.value)
			op := cond.op
			if op == "" {
				op = "=="
			}
			terms.albumRating = &ratingCond{op: op, value: v}
		case "added-since":
			ts, err := parseTimeArg(cond.value)
			if err != nil {
				terms.unsupportedT = "invalid timestamp for added-since"
				return terms
			}
			terms.addedSince = ts
		case "rating", "x-rating":
			v, _ := strconv.Atoi(cond.value)
			op := cond.op
			if op == "" {
				op = "=="
			}
			terms.trackRating = &ratingCond{op: op, value: v}
		case "any":
			terms.textWords = append(terms.textWords, cond.value)
		default:
			if isTechnicalConditionTag(cond.tag) {
				terms.technical = append(terms.technical, cond)
				continue
			}
			if _, ok := mpdTagNames[cond.tag]; ok {
				terms.trackLevel = append(terms.trackLevel, cond)
				continue
			}
			switch cond.tag {
			case "artist", "title":
				terms.trackLevel = append(terms.trackLevel, cond)
			default:
				terms.unsupportedT = "unsupported searchalbums filter tag: " + cond.tag
				return terms
			}
		}
	}
	return terms
}

// matchAlbumText applies one album-level string condition with search's
// case-insensitive semantics ("==" exact, "contains" substring).
func matchAlbumText(cond *filterCondition, value string) bool {
	if cond == nil {
		return true
	}
	switch cond.op {
	case "contains":
		return strings.Contains(strings.ToLower(value), strings.ToLower(cond.value))
	default:
		return strings.EqualFold(value, cond.value)
	}
}

// trackLevelAlbumIDs resolves the track-scoped terms to the set of album ids
// containing at least one matching track. The second result reports whether
// track-level filtering applies at all.
func trackLevelAlbumIDs(a *app, terms albumSearchTerms) (map[string]bool, bool, *mpdError) {
	hasTrackTerms := len(terms.trackLevel) > 0 || terms.trackRating != nil ||
		len(terms.textWords) > 0 || len(terms.technical) > 0
	if !hasTrackTerms {
		return nil, false, nil
	}

	var tracks []map[string]any
	var err error
	switch {
	case len(terms.textWords) > 0:
		// Text terms narrow through the shared search index first; the
		// remaining structured terms filter the hits below.
		_, tracks, err = a.db.search(strings.Join(terms.textWords, " "), 1000)
	case len(terms.trackLevel) > 0:
		var generic []filterCondition
		var artist, title string
		for _, cond := range terms.trackLevel {
			switch cond.tag {
			case "artist":
				artist = cond.value
			case "title":
				title = cond.value
			default:
				generic = append(generic, cond)
			}
		}
		tracks, err = a.db.tracksByConditions(generic, artist, "", "", "", true)
		if err == nil && title != "" {
			var kept []map[string]any
			for _, t := range tracks {
				if strings.EqualFold(stringify(t["title"]), title) {
					kept = append(kept, t)
				}
			}
			tracks = kept
		}
	case terms.trackRating != nil:
		tracks, err = a.db.tracksByRatingOp(terms.trackRating.op, terms.trackRating.value)
	default:
		// Technical-only track terms scan the library once.
		tracks, err = a.db.allTracks()
	}
	if err != nil {
		return nil, true, mpdErr(errSystem, "searchalbums", err.Error())
	}
	tracks = filterTracksByTechnicals(tracks, terms.technical)

	ids := map[string]bool{}
	for _, t := range tracks {
		if terms.trackRating != nil {
			if r := intFromAny(t["rating"], 0); !compareRating(r, terms.trackRating.op, terms.trackRating.value) {
				continue
			}
		}
		if id := stringify(t["album_id"]); id != "" {
			ids[id] = true
		}
	}
	return ids, true, nil
}

type albumRecord struct {
	id     int64
	artist string
	album  string
	date   string
	added  int64
	rating int
}

func cmdSearchAlbums(c *mpdConn, args []string) *mpdError {
	if len(args) < 1 {
		return mpdErr(errArg, "searchalbums", "need filter arguments")
	}

	// Extract sort and window exactly like search does.
	var windowStart, windowEnd = 0, -1
	var sortKey string
	var sortDesc bool
	var filterArgs []string
	for i := 0; i < len(args); i++ {
		if strings.ToLower(args[i]) == "window" && i+1 < len(args) {
			parts := strings.SplitN(args[i+1], ":", 2)
			if len(parts) == 2 {
				windowStart, _ = strconv.Atoi(parts[0])
				windowEnd, _ = strconv.Atoi(parts[1])
			}
			i++
			continue
		}
		if strings.ToLower(args[i]) == "sort" && i+1 < len(args) {
			key := args[i+1]
			if strings.HasPrefix(key, "-") {
				sortDesc = true
				key = key[1:]
			}
			sortKey = strings.ToLower(key)
			if !albumSortKeys[sortKey] {
				return mpdErr(errArg, "searchalbums", "Unsupported sort tag")
			}
			i++
			continue
		}
		filterArgs = append(filterArgs, args[i])
	}
	if len(filterArgs) == 0 {
		return mpdErr(errArg, "searchalbums", "need filter arguments")
	}

	var trees []*filterNode
	for _, arg := range filterArgs {
		if !strings.HasPrefix(arg, "(") {
			return mpdErr(errArg, "searchalbums", "need filter expressions")
		}
		tree, err := parseFilterTree(arg)
		if err != nil {
			return mpdErr(errArg, "searchalbums", err.Error())
		}
		trees = append(trees, tree)
	}
	tree := trees[0]
	if len(trees) > 1 {
		tree = &filterNode{kind: filterAnd, children: trees}
	}

	a := c.app
	conditions, flat := flattenConjunction(tree)
	var terms albumSearchTerms
	if flat {
		terms = splitAlbumSearchTerms(conditions)
		if terms.unsupportedT != "" {
			return mpdErr(errArg, "searchalbums", terms.unsupportedT)
		}
	}

	albums, err := a.db.allAlbums(false)
	if err != nil {
		return mpdErr(errSystem, "searchalbums", err.Error())
	}
	var trackAlbumIDs map[string]bool
	var trackFiltered bool
	if flat {
		var mpdErr2 *mpdError
		trackAlbumIDs, trackFiltered, mpdErr2 = trackLevelAlbumIDs(a, terms)
		if mpdErr2 != nil {
			return mpdErr2
		}
	} else {
		// Structured expressions evaluate per track; an album matches when
		// any of its tracks does. Album-level fields (albumartist, date,
		// albumrating, ...) resolve fine per track, so this covers the
		// whole grammar.
		tracks, err := a.db.allTracks()
		if err != nil {
			return mpdErr(errSystem, "searchalbums", err.Error())
		}
		env := newFilterEnv(a, true)
		trackAlbumIDs = map[string]bool{}
		for _, t := range tracks {
			if env.matches(tree, t) {
				trackAlbumIDs[stringify(t["album_id"])] = true
			}
		}
		trackFiltered = true
	}

	// Stored album ratings for filtering, sorting, and the response lines.
	hashes := make([]string, len(albums))
	for i, al := range albums {
		hashes[i] = albumRatingHash(stringify(al["albumartist"]), stringify(al["album"]),
			stringify(al["date"]))
	}
	ratings, err := a.db.getRatingsBatch(hashes)
	if err != nil {
		return mpdErr(errSystem, "searchalbums", err.Error())
	}

	var matched []albumRecord
	for i, al := range albums {
		record := albumRecord{
			artist: stringify(al["albumartist"]),
			album:  stringify(al["album"]),
			date:   stringify(al["date"]),
			added:  int64(intFromAny(al["added"], 0)),
			rating: ratings[hashes[i]],
		}
		record.id, _ = strconv.ParseInt(stringify(al["id"]), 10, 64)
		if !matchAlbumText(terms.albumArtist, record.artist) ||
			!matchAlbumText(terms.album, record.album) ||
			!matchAlbumText(terms.date, record.date) {
			continue
		}
		if terms.albumRating != nil &&
			!compareRating(record.rating, terms.albumRating.op, terms.albumRating.value) {
			continue
		}
		if terms.addedSince != 0 && record.added < terms.addedSince {
			continue
		}
		if trackFiltered && !trackAlbumIDs[stringify(al["id"])] {
			continue
		}
		matched = append(matched, record)
	}

	if sortKey != "" {
		sort.SliceStable(matched, func(i, j int) bool {
			left, right := matched[i], matched[j]
			if sortDesc {
				left, right = right, left
			}
			switch sortKey {
			case "album":
				return strings.ToLower(left.album) < strings.ToLower(right.album)
			case "date":
				return left.date < right.date
			case "added":
				return left.added < right.added
			case "rating":
				return left.rating < right.rating
			default:
				return strings.ToLower(left.artist) < strings.ToLower(right.artist)
			}
		})
	}

	if windowEnd >= 0 {
		if windowStart < 0 {
			windowStart = 0
		}
		if windowStart > len(matched) {
			windowStart = len(matched)
		}
		if windowEnd > len(matched) {
			windowEnd = len(matched)
		}
		matched = matched[windowStart:windowEnd]
	}

	for _, record := range matched {
		// One bounded per-album track read supplies count, duration,
		// computed rating, and the representative artwork path.
		tracks, err := a.db.tracksByAlbum(record.id)
		if err != nil {
			return mpdErr(errSystem, "searchalbums", err.Error())
		}
		var duration float64
		sum, rated := 0, 0
		artwork := ""
		for _, t := range tracks {
			duration += floatFromAny(t["duration"], 0)
			if r := intFromAny(t["rating"], 0); r > 0 {
				sum += r
				rated++
			}
			if artwork == "" {
				artwork = stringify(t["path"])
			}
		}
		c.writeKV("AlbumArtist", record.artist)
		c.writeKV("Album", record.album)
		// The full stored date, including 0000, so clients can address the
		// exact album identity in albumrate/getalbumrating.
		c.writeKV("Date", record.date)
		c.writeKV("X-AlbumId", record.id)
		c.writeKV("X-TrackCount", len(tracks))
		c.writeKV("X-Duration", int64(duration))
		if record.rating > 0 {
			c.writeKV("X-Rating", record.rating)
		}
		if total := len(tracks); total > 0 && rated > 0 &&
			float64(rated)/float64(total) >= 0.7 {
			c.writef("X-ComputedRating: %.1f\n", float64(sum)/float64(rated))
		}
		if artwork != "" {
			c.writeKV("X-ArtworkUri", c.pathToURI(artwork))
		}
	}
	return nil
}
