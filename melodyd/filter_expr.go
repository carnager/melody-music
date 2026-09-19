package main

import (
	"errors"
	"path/filepath"
	"strconv"
	"strings"
)

// Filter expression trees (docs/protocol.md). The grammar follows MPD's
// filter syntax — parenthesized expressions, AND conjunctions, and "!"
// negation — plus Melody's OR extension. Advertised via the filtergrammar
// command; flat AND-of-leaves expressions keep the specialized fast paths,
// everything else evaluates per track through filterEnv.
//
//	EXPR  := '(' INNER ')'
//	INNER := '!' EXPR
//	       | EXPR ( 'AND' EXPR )+ | EXPR ( 'OR' EXPR )+
//	       | TAG OP VALUE | base VALUE | added-since VALUE | modified-since VALUE
//	OP    := '==' | '!=' | 'contains' | '>=' | '<=' | '>' | '<'

type filterNodeKind int

const (
	filterLeaf filterNodeKind = iota
	filterAnd
	filterOr
	filterNot
)

type filterNode struct {
	kind     filterNodeKind
	cond     filterCondition
	children []*filterNode
}

// scanTopLevel walks s and reports the byte ranges of top-level " AND " /
// " OR " connectors, honoring nesting and quoting.
type connectorSplit struct {
	segments  []string
	connector string
}

func splitTopLevel(s string) (connectorSplit, error) {
	var split connectorSplit
	depth := 0
	var quote byte
	segmentStart := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if quote != 0 {
			if c == '\\' {
				i++
			} else if c == quote {
				quote = 0
			}
			continue
		}
		switch c {
		case '\'', '"':
			quote = c
		case '(':
			depth++
		case ')':
			depth--
			if depth < 0 {
				return split, errors.New("unbalanced parentheses in filter")
			}
		case ' ':
			if depth != 0 {
				continue
			}
			var connector string
			if strings.HasPrefix(s[i:], " AND ") {
				connector = "AND"
			} else if strings.HasPrefix(s[i:], " OR ") {
				connector = "OR"
			} else {
				continue
			}
			if split.connector == "" {
				split.connector = connector
			} else if split.connector != connector {
				return split, errors.New("mixing AND and OR needs explicit parentheses")
			}
			split.segments = append(split.segments, s[segmentStart:i])
			i += len(connector) + 1
			segmentStart = i + 1
		}
	}
	if quote != 0 {
		return split, errors.New("unterminated quote in filter")
	}
	if depth != 0 {
		return split, errors.New("unbalanced parentheses in filter")
	}
	if split.connector != "" {
		split.segments = append(split.segments, s[segmentStart:])
	}
	return split, nil
}

// parseLeaf parses "TAG OP VALUE" (and the space-separated specials) with a
// quote-aware operator search, so values containing operator spellings stay
// intact.
func parseLeaf(s string) (filterCondition, error) {
	s = strings.TrimSpace(s)
	depth := 0
	var quote byte
	for i := 0; i < len(s); i++ {
		c := s[i]
		if quote != 0 {
			if c == '\\' {
				i++
			} else if c == quote {
				quote = 0
			}
			continue
		}
		switch c {
		case '\'', '"':
			quote = c
		case '(':
			depth++
		case ')':
			depth--
		case ' ':
			if depth != 0 {
				continue
			}
			for _, op := range []string{">= ", "<= ", "== ", "!= ", "> ", "< ", "contains "} {
				if strings.HasPrefix(s[i+1:], op) {
					tag := strings.ToLower(strings.TrimSpace(s[:i]))
					value := stripQuotes(strings.TrimSpace(s[i+1+len(op):]))
					if tag == "" {
						return filterCondition{}, errors.New("filter condition needs a tag")
					}
					return filterCondition{tag: tag, op: strings.TrimSpace(op), value: value}, nil
				}
			}
			// Specials without an operator: base, added-since, modified-since.
			tag := strings.ToLower(strings.TrimSpace(s[:i]))
			switch tag {
			case "base", "added-since", "modified-since":
				return filterCondition{tag: tag,
					value: stripQuotes(strings.TrimSpace(s[i+1:]))}, nil
			}
			return filterCondition{}, errors.New("unknown filter operator after " + tag)
		}
	}
	return filterCondition{}, errors.New("incomplete filter condition")
}

// parseFilterTree parses one parenthesized filter expression.
func parseFilterTree(expr string) (*filterNode, error) {
	expr = strings.TrimSpace(expr)
	if len(expr) < 2 || expr[0] != '(' || expr[len(expr)-1] != ')' {
		return nil, errors.New("filter expressions are parenthesized")
	}
	inner := strings.TrimSpace(expr[1 : len(expr)-1])
	if inner == "" {
		return nil, errors.New("empty filter expression")
	}
	if inner[0] == '!' {
		child, err := parseFilterTree(strings.TrimSpace(inner[1:]))
		if err != nil {
			return nil, err
		}
		return &filterNode{kind: filterNot, children: []*filterNode{child}}, nil
	}
	split, err := splitTopLevel(inner)
	if err != nil {
		return nil, err
	}
	if split.connector != "" {
		node := &filterNode{kind: filterAnd}
		if split.connector == "OR" {
			node.kind = filterOr
		}
		for _, segment := range split.segments {
			child, err := parseFilterTree(strings.TrimSpace(segment))
			if err != nil {
				return nil, err
			}
			node.children = append(node.children, child)
		}
		return node, nil
	}
	if inner[0] == '(' {
		// A redundant grouping layer around a single expression.
		return parseFilterTree(inner)
	}
	cond, err := parseLeaf(inner)
	if err != nil {
		return nil, err
	}
	return &filterNode{kind: filterLeaf, cond: cond}, nil
}

// parseFilterConditions is the tolerant flat view older call sites expect:
// a parseable AND-of-leaves expression becomes its condition list, anything
// else (errors, OR, negation) becomes nil.
func parseFilterConditions(expr string) []filterCondition {
	tree, err := parseFilterTree(expr)
	if err != nil {
		return nil
	}
	conditions, ok := flattenConjunction(tree)
	if !ok {
		return nil
	}
	return conditions
}

// flattenConjunction reports the tree as a legacy condition list when it is
// a plain AND of leaves the specialized paths understand; "!=" and the
// empty-value present/missing forms only exist in the evaluator.
func flattenConjunction(node *filterNode) ([]filterCondition, bool) {
	var conditions []filterCondition
	var walk func(*filterNode) bool
	walk = func(n *filterNode) bool {
		switch n.kind {
		case filterLeaf:
			cond := n.cond
			switch cond.tag {
			case "base", "file", "filename":
				// Path conditions stay legacy even with empty values.
				conditions = append(conditions, cond)
				return true
			}
			switch cond.op {
			case "!=":
				return false
			case ">", ">=", "<", "<=":
				// The fast paths only compare ratings and technicals;
				// ranges on ordinary tags need the evaluator.
				switch {
				case cond.tag == "rating" || cond.tag == "x-rating" ||
					cond.tag == "albumrating":
				case isTechnicalConditionTag(cond.tag):
				default:
					return false
				}
			default:
				if cond.value == "" {
					// Empty-value present/missing forms are evaluator-only.
					return false
				}
			}
			conditions = append(conditions, cond)
			return true
		case filterAnd:
			for _, child := range n.children {
				if !walk(child) {
					return false
				}
			}
			return true
		default:
			return false
		}
	}
	if !walk(node) {
		return nil, false
	}
	return conditions, true
}

// filterEnv evaluates a tree per track, caching album ratings by identity.
type filterEnv struct {
	a               *app
	caseInsensitive bool
	albumRatings    map[string]int
}

func newFilterEnv(a *app, caseInsensitive bool) *filterEnv {
	return &filterEnv{a: a, caseInsensitive: caseInsensitive, albumRatings: map[string]int{}}
}

func (env *filterEnv) albumRating(track map[string]any) int {
	hash := albumRatingHash(stringify(track["albumartist"]), stringify(track["album"]),
		stringify(track["date"]))
	if rating, ok := env.albumRatings[hash]; ok {
		return rating
	}
	rating, _ := env.a.db.getRating(hash)
	env.albumRatings[hash] = rating
	return rating
}

func (env *filterEnv) matches(node *filterNode, track map[string]any) bool {
	switch node.kind {
	case filterNot:
		return !env.matches(node.children[0], track)
	case filterAnd:
		for _, child := range node.children {
			if !env.matches(child, track) {
				return false
			}
		}
		return true
	case filterOr:
		for _, child := range node.children {
			if env.matches(child, track) {
				return true
			}
		}
		return false
	default:
		return env.leafMatches(node.cond, track)
	}
}

// trackFieldValues returns the values a tag names on this track: dedicated
// columns first, then the generic tag table.
func trackFieldValues(track map[string]any, tag string) []string {
	switch tag {
	case "artist", "albumartist", "album", "title", "date":
		if value := stringify(track[tag]); value != "" {
			return []string{value}
		}
		return nil
	}
	if tags, ok := track["tags"].(map[string][]string); ok {
		return tags[tag]
	}
	return nil
}

func (env *filterEnv) textEquals(left, right string) bool {
	if env.caseInsensitive {
		return strings.EqualFold(left, right)
	}
	return left == right
}

func (env *filterEnv) textContains(haystack, needle string) bool {
	if env.caseInsensitive {
		return strings.Contains(strings.ToLower(haystack), strings.ToLower(needle))
	}
	return strings.Contains(haystack, needle)
}

func leadingInteger(value string) (int, bool) {
	end := 0
	for end < len(value) && value[end] >= '0' && value[end] <= '9' {
		end++
	}
	if end == 0 {
		return 0, false
	}
	number, err := strconv.Atoi(value[:end])
	return number, err == nil
}

func (env *filterEnv) leafMatches(cond filterCondition, track map[string]any) bool {
	switch cond.tag {
	case "any":
		haystacks := []string{stringify(track["artist"]), stringify(track["albumartist"]),
			stringify(track["title"]), stringify(track["album"])}
		if tags, ok := track["tags"].(map[string][]string); ok {
			for _, values := range tags {
				haystacks = append(haystacks, values...)
			}
		}
		for _, haystack := range haystacks {
			if env.textContains(haystack, cond.value) {
				return true
			}
		}
		return false
	case "base":
		if cond.value == "" {
			return true
		}
		prefix := filepath.Join(env.a.cfg.Library.MusicDir, cond.value) +
			string(filepath.Separator)
		return strings.HasPrefix(stringify(track["path"]), prefix)
	case "added-since":
		cutoff, err := parseTimeArg(cond.value)
		return err == nil && int64(intFromAny(track["added"], 0)) >= cutoff
	case "modified-since":
		cutoff, err := parseTimeArg(cond.value)
		// file_modified is unix milliseconds; the cutoff is seconds.
		return err == nil && int64(intFromAny(track["file_modified"], 0)) >= cutoff*1000
	case "rating", "x-rating":
		operand, err := strconv.Atoi(cond.value)
		if err != nil {
			return false
		}
		return compareRating(intFromAny(track["rating"], 0), orEquals(cond.op), operand)
	case "albumrating":
		operand, err := strconv.Atoi(cond.value)
		if err != nil {
			return false
		}
		return compareRating(env.albumRating(track), orEquals(cond.op), operand)
	case "file", "filename":
		relative := strings.TrimPrefix(stringify(track["path"]),
			env.a.cfg.Library.MusicDir+string(filepath.Separator))
		if cond.value == "" {
			return true
		}
		if cond.op == "contains" {
			return env.textContains(relative, cond.value)
		}
		return env.textEquals(relative, cond.value)
	}
	if isTechnicalConditionTag(cond.tag) {
		return matchTechnicalCondition(track, cond)
	}

	values := trackFieldValues(track, cond.tag)
	switch cond.op {
	case "==":
		if cond.value == "" {
			// MPD's absent-tag form.
			return len(values) == 0
		}
		for _, value := range values {
			if env.textEquals(value, cond.value) {
				return true
			}
		}
		return false
	case "!=":
		if cond.value == "" {
			return len(values) > 0
		}
		if len(values) == 0 {
			return false
		}
		for _, value := range values {
			if env.textEquals(value, cond.value) {
				return false
			}
		}
		return true
	case "contains":
		for _, value := range values {
			if env.textContains(value, cond.value) {
				return true
			}
		}
		return false
	case ">", ">=", "<", "<=", "":
		operand, err := strconv.Atoi(cond.value)
		if err != nil {
			return false
		}
		for _, value := range values {
			if number, ok := leadingInteger(value); ok &&
				compareRating(number, orEquals(cond.op), operand) {
				return true
			}
		}
		return false
	}
	return false
}

func orEquals(op string) string {
	if op == "" {
		return "=="
	}
	return op
}

// cmdFindByTree answers structured expressions the flat fast paths cannot:
// one bounded library pass with per-track evaluation, then the shared
// sort/window/write pipeline.
func cmdFindByTree(c *mpdConn, tree *filterNode, cmdName string, caseInsensitive,
	addToQueue bool) *mpdError {
	tracks, err := c.app.db.allTracks()
	if err != nil {
		return mpdErr(errSystem, cmdName, err.Error())
	}
	env := newFilterEnv(c.app, caseInsensitive)
	var matched []map[string]any
	for _, track := range tracks {
		if env.matches(tree, track) {
			matched = append(matched, track)
		}
	}
	return writeOrAddFilteredTracks(c, c.app, matched, nil, nil, cmdName, addToQueue)
}

// cmdFilterGrammar advertises the extended filter grammar level.
func cmdFilterGrammar(c *mpdConn, args []string) *mpdError {
	c.writeKV("grammar", 2)
	return nil
}
