package main

// Generic tag support: the scanner extracts all recognized file tags (genre,
// composer, MusicBrainz IDs, ...) into the track_tags table, and the MPD
// layer exposes them via tagtypes, track responses, list, and find/search.

import (
	"strings"

	"github.com/dhowden/tag"
)

// mpdTagNames maps canonical (lowercase) tag names to their MPD protocol
// spelling. Every tag stored in track_tags uses a canonical name from this map.
// The slice fixes the emission order in track responses and tagtypes.
var mpdTagOrder = []string{
	"genre",
	"composer",
	"performer",
	"conductor",
	"ensemble",
	"work",
	"grouping",
	"mood",
	"label",
	"comment",
	"originaldate",
	"location",
	"artistsort",
	"albumartistsort",
	"albumsort",
	"titlesort",
	"musicbrainz_artistid",
	"musicbrainz_albumid",
	"musicbrainz_albumartistid",
	"musicbrainz_trackid",
	"musicbrainz_releasetrackid",
	"musicbrainz_releasegroupid",
	"musicbrainz_workid",
}

var mpdTagNames = map[string]string{
	"genre":                      "Genre",
	"composer":                   "Composer",
	"performer":                  "Performer",
	"conductor":                  "Conductor",
	"ensemble":                   "Ensemble",
	"work":                       "Work",
	"grouping":                   "Grouping",
	"mood":                       "Mood",
	"label":                      "Label",
	"comment":                    "Comment",
	"originaldate":               "OriginalDate",
	"location":                   "Location",
	"artistsort":                 "ArtistSort",
	"albumartistsort":            "AlbumArtistSort",
	"albumsort":                  "AlbumSort",
	"titlesort":                  "TitleSort",
	"musicbrainz_artistid":       "MUSICBRAINZ_ARTISTID",
	"musicbrainz_albumid":        "MUSICBRAINZ_ALBUMID",
	"musicbrainz_albumartistid":  "MUSICBRAINZ_ALBUMARTISTID",
	"musicbrainz_trackid":        "MUSICBRAINZ_TRACKID",
	"musicbrainz_releasetrackid": "MUSICBRAINZ_RELEASETRACKID",
	"musicbrainz_releasegroupid": "MUSICBRAINZ_RELEASEGROUPID",
	"musicbrainz_workid":         "MUSICBRAINZ_WORKID",
}

// rawTagAliases maps lowercased raw tag keys — Vorbis comment names, MP4 atom
// names as decoded by dhowden/tag, iTunes freeform names, and ID3v2 TXXX
// descriptions — to canonical names. ID3v2 frame IDs are handled separately in
// id3FrameTags. Note: in both Vorbis and MPD, MUSICBRAINZ_TRACKID is the
// *recording* MBID; MUSICBRAINZ_RELEASETRACKID is the release-track MBID.
var rawTagAliases = map[string]string{
	"genre":        "genre",
	"composer":     "composer",
	"performer":    "performer",
	"conductor":    "conductor",
	"ensemble":     "ensemble",
	"work":         "work",
	"grouping":     "grouping",
	"mood":         "mood",
	"label":        "label",
	"organization": "label",
	"comment":      "comment",
	"description":  "comment",
	"originaldate": "originaldate",
	"location":     "location",

	"artistsort":        "artistsort",
	"artist sort":       "artistsort",
	"albumartistsort":   "albumartistsort",
	"album artist sort": "albumartistsort",
	"albumsort":         "albumsort",
	"album sort":        "albumsort",
	"titlesort":         "titlesort",
	"title sort":        "titlesort",

	"musicbrainz_artistid":         "musicbrainz_artistid",
	"musicbrainz artist id":        "musicbrainz_artistid",
	"musicbrainz_albumid":          "musicbrainz_albumid",
	"musicbrainz album id":         "musicbrainz_albumid",
	"musicbrainz_albumartistid":    "musicbrainz_albumartistid",
	"musicbrainz album artist id":  "musicbrainz_albumartistid",
	"musicbrainz_trackid":          "musicbrainz_trackid",
	"musicbrainz track id":         "musicbrainz_trackid",
	"musicbrainz_releasetrackid":   "musicbrainz_releasetrackid",
	"musicbrainz release track id": "musicbrainz_releasetrackid",
	"musicbrainz_releasegroupid":   "musicbrainz_releasegroupid",
	"musicbrainz release group id": "musicbrainz_releasegroupid",
	"musicbrainz_workid":           "musicbrainz_workid",
	"musicbrainz work id":          "musicbrainz_workid",
}

// id3FrameTags maps ID3v2.3/2.4 text frame IDs to canonical tag names.
// TXXX and UFID frames are resolved through rawTagAliases instead.
var id3FrameTags = map[string]string{
	"TCON": "genre",
	"TCO":  "genre", // ID3v2.2
	"TCOM": "composer",
	"TCM":  "composer", // ID3v2.2
	"TPE3": "conductor",
	"TIT1": "grouping",
	"GRP1": "grouping",
	"TPUB": "label",
	"TMOO": "mood",
	"TDOR": "originaldate",
	"TSOP": "artistsort",
	"TSO2": "albumartistsort",
	"TSOA": "albumsort",
	"TSOT": "titlesort",
}

// UFID provider URL used by MusicBrainz Picard for the recording MBID.
const mbUFIDProvider = "http://musicbrainz.org"

const (
	maxTagValueLen  = 512
	maxValuesPerTag = 32
)

// normalizeRawTags extracts all recognized tags from file metadata into a
// canonical-name → values map. Values are split on NUL (ID3v2.4 multi-value)
// and ";" (common join for multi-value genre and dhowden's MP4 join),
// trimmed, deduplicated, and length-capped.
func normalizeRawTags(m tag.Metadata) map[string][]string {
	out := map[string][]string{}
	add := func(canonical, raw string) {
		if canonical == "" || raw == "" {
			return
		}
		for _, v := range splitTagValue(raw) {
			vals := out[canonical]
			if len(vals) >= maxValuesPerTag {
				return
			}
			dup := false
			for _, existing := range vals {
				if strings.EqualFold(existing, v) {
					dup = true
					break
				}
			}
			if !dup {
				out[canonical] = append(vals, v)
			}
		}
	}

	isID3 := false
	switch m.Format() {
	case tag.ID3v2_2, tag.ID3v2_3, tag.ID3v2_4:
		isID3 = true
	}

	for key, val := range m.Raw() {
		// Duplicate frames/atoms are stored as "NAME_0", "NAME_1", ...
		if i := strings.IndexByte(key, '_'); isID3 && i > 0 {
			key = key[:i]
		}
		switch v := val.(type) {
		case string:
			if isID3 {
				add(id3FrameTags[key], v)
			} else {
				add(rawTagAliases[strings.ToLower(key)], v)
			}
		case *tag.Comm:
			// ID3v2 TXXX (user-defined text) and COMM (comment) frames.
			switch key {
			case "TXXX", "TXX":
				add(rawTagAliases[strings.ToLower(v.Description)], v.Text)
			case "COMM", "COM":
				add("comment", v.Text)
			}
		case *tag.UFID:
			if v.Provider == mbUFIDProvider {
				add("musicbrainz_trackid", string(v.Identifier))
			}
		}
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

func splitTagValue(raw string) []string {
	var vals []string
	for _, part := range strings.FieldsFunc(raw, func(r rune) bool {
		return r == 0 || r == ';'
	}) {
		v := strings.TrimSpace(part)
		if v == "" {
			continue
		}
		if len(v) > maxTagValueLen {
			continue
		}
		vals = append(vals, v)
	}
	return vals
}
