package main

import (
	"strings"
	"testing"
)

func TestParseFilterTree(t *testing.T) {
	// A quoted value containing a connector spelling stays one leaf.
	tree, err := parseFilterTree(`(Album == "Trouble AND Strife")`)
	if err != nil {
		t.Fatalf("parseFilterTree: %v", err)
	}
	if tree.kind != filterLeaf || tree.cond.tag != "album" ||
		tree.cond.value != "Trouble AND Strife" {
		t.Fatalf("quoted AND leaf = %+v", tree)
	}

	// Nested negation and disjunction build the expected shape.
	tree, err = parseFilterTree(`((!(Genre == "Jazz")) OR (Artist contains "beta"))`)
	if err != nil {
		t.Fatalf("parseFilterTree: %v", err)
	}
	if tree.kind != filterOr || len(tree.children) != 2 ||
		tree.children[0].kind != filterNot ||
		tree.children[0].children[0].cond.tag != "genre" {
		t.Fatalf("nested tree = %+v", tree)
	}

	// Flat conjunctions stay compatible with the legacy condition list.
	conditions := parseFilterConditions(`((AlbumArtist == "x") AND (Album == "y"))`)
	if len(conditions) != 2 || conditions[0].tag != "albumartist" || conditions[1].value != "y" {
		t.Fatalf("flat conditions = %+v", conditions)
	}

	for _, bad := range []string{
		`(A == "x") AND (B == "y")`,                 // missing outer parens
		`((A == "x") AND (B == "y") OR (C == "z"))`, // mixed connectors
		`((A == "x")`,          // unbalanced
		`(Album == "unclosed)`, // unterminated quote
		`(Album ~= "x")`,       // unknown operator
		`()`,                   // empty
	} {
		if _, err := parseFilterTree(bad); err == nil {
			t.Fatalf("parseFilterTree(%q) should fail", bad)
		}
	}
}

func TestFindStructuredExpressions(t *testing.T) {
	a, _ := newSearchAlbumsApp(t)

	// OR unions the branches (Melody extension).
	out := dispatchCapture(t, a,
		`find "((Artist == \"Alpha Artist\") OR (Artist == \"Beta Artist\"))"`)
	if got := fileOrder(out); len(got) != 3 {
		t.Fatalf("OR union = %v, want 3 tracks\n%s", got, out)
	}

	// Stock-MPD negation.
	out = dispatchCapture(t, a, `find "(!(Genre == \"Jazz\"))"`)
	if got := fileOrder(out); len(got) != 2 || strings.Contains(out, "Opening") {
		t.Fatalf("negation = %v, want Only and Mystery\n%s", got, out)
	}

	// Empty-value forms: absent tag and present tag.
	out = dispatchCapture(t, a, `find "(Genre == \"\")"`)
	if got := fileOrder(out); len(got) != 2 || strings.Contains(out, "Closing") {
		t.Fatalf("missing genre = %v, want Only and Mystery\n%s", got, out)
	}
	out = dispatchCapture(t, a, `find "(Genre != \"\")"`)
	if got := fileOrder(out); len(got) != 2 || !strings.Contains(out, "Opening") {
		t.Fatalf("present genre = %v, want Opening and Closing\n%s", got, out)
	}

	// != with a value means present-and-different.
	out = dispatchCapture(t, a, `find "(Artist != \"Alpha Artist\")"`)
	if got := fileOrder(out); len(got) != 2 || strings.Contains(out, "Opening") {
		t.Fatalf("artist != = %v, want Only and Mystery\n%s", got, out)
	}

	// Numeric comparisons on ordinary fields use the leading integer.
	out = dispatchCapture(t, a, `find "((date > 1995) AND (date < 2010))"`)
	if got := fileOrder(out); len(got) != 1 || !strings.Contains(out, "Only") {
		t.Fatalf("date range = %v, want Only\n%s", got, out)
	}

	// Nested groups compose.
	out = dispatchCapture(t, a,
		`find "(((Artist == \"Alpha Artist\") AND (Title == \"Opening\")) OR (Title == \"Only\"))"`)
	if got := fileOrder(out); len(got) != 2 || strings.Contains(out, "Closing") {
		t.Fatalf("nested tree = %v, want Opening and Only\n%s", got, out)
	}

	// Sort and window apply to structured results too.
	out = dispatchCapture(t, a,
		`find "((Artist == \"Alpha Artist\") OR (Artist == \"Beta Artist\"))" sort -title window 0:1`)
	if got := fileOrder(out); len(got) != 1 || !strings.Contains(got[0], "Opening") {
		t.Fatalf("sort -title window 0:1 = %v, want Opening\n%s", got, out)
	}

	// find is exact-case, search folds case — for trees like for flat filters.
	if got := fileOrder(dispatchCapture(t, a, `find "(!(Artist == \"alpha artist\"))"`)); len(got) != 4 {
		t.Fatalf("case-sensitive find = %v, want all 4\n", got)
	}
	if got := fileOrder(dispatchCapture(t, a, `search "(!(Artist == \"alpha artist\"))"`)); len(got) != 2 {
		t.Fatalf("case-insensitive search = %v, want 2\n", got)
	}

	// Malformed expressions are protocol errors, not empty result sets.
	if err := dispatchError(t, a, `find "((A == \"x\") AND (B == \"y\") OR (C == \"z\"))"`); err == nil {
		t.Fatalf("mixed AND/OR must error")
	}
}

func TestSearchAlbumsStructuredExpressions(t *testing.T) {
	a, _ := newSearchAlbumsApp(t)

	out := dispatchCapture(t, a,
		`searchalbums "((Genre == \"Jazz\") OR (albumartist == \"Beta Artist\"))"`)
	if got := albumOrder(out); strings.Join(got, ",") != "First Album,Second Album" {
		t.Fatalf("searchalbums OR = %v, want First Album,Second Album\n%s", got, out)
	}

	out = dispatchCapture(t, a, `searchalbums "(!(Genre == \"Jazz\"))"`)
	if got := albumOrder(out); strings.Join(got, ",") != "Second Album,Undated Album" {
		t.Fatalf("searchalbums negation = %v, want Second Album,Undated Album\n%s", got, out)
	}

	if err := dispatchError(t, a, `searchalbums "((Genre == \"Jazz\") OR"`); err == nil {
		t.Fatalf("malformed searchalbums filter must error")
	}
}

func TestFilterGrammarCapability(t *testing.T) {
	a, _ := newSearchAlbumsApp(t)
	out := dispatchCapture(t, a, `filtergrammar`)
	if !strings.Contains(out, "grammar: 2") {
		t.Fatalf("filtergrammar = %q, want grammar: 2", out)
	}
	if !strings.Contains(dispatchCapture(t, a, `commands`), "command: filtergrammar") {
		t.Fatalf("filtergrammar must be advertised in commands")
	}
}
