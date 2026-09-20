// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"strings"
	"testing"
)

func TestTrackTagLookupUsesCoveringIndexAfterUpgrade(t *testing.T) {
	a, _ := newSearchAlbumsApp(t)
	// Recreate the schema from before the upgrade, keeping real tag rows.
	if _, err := a.db.db.Exec(`DROP INDEX idx_track_tags_tag_value_track;
        CREATE INDEX idx_track_tags_tag_value ON track_tags(tag, value)`); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		if err := a.db.migrate(); err != nil {
			t.Fatal(err)
		}
	}
	rows, err := a.db.db.Query(`EXPLAIN QUERY PLAN SELECT track_id FROM track_tags
        WHERE tag = ? AND value = ?`, "genre", "Jazz")
	if err != nil {
		t.Fatal(err)
	}
	var details []string
	for rows.Next() {
		var id, parent, unused int
		var detail string
		if err := rows.Scan(&id, &parent, &unused, &detail); err != nil {
			t.Fatal(err)
		}
		details = append(details, detail)
	}
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}
	rows.Close()
	if !strings.Contains(strings.Join(details, "\n"),
		"USING COVERING INDEX idx_track_tags_tag_value_track (tag=? AND value=?)") {
		t.Fatalf("lookup is not covered: %v", details)
	}
	var legacy int
	if err := a.db.db.QueryRow(`SELECT COUNT(*) FROM sqlite_master
        WHERE type='index' AND name='idx_track_tags_tag_value'`).Scan(&legacy); err != nil {
		t.Fatal(err)
	}
	if legacy != 0 {
		t.Fatal("redundant legacy index remains")
	}
	tracks, narrowed, err := a.db.tracksMatching("genre", "Jazz", false)
	if err != nil || !narrowed || len(tracks) != 2 {
		t.Fatalf("genre lookup after upgrade: tracks=%v narrowed=%v err=%v", tracks, narrowed, err)
	}
}
