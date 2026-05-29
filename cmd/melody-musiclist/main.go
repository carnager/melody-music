package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/BurntSushi/toml"
)

type config struct {
	MPD struct {
		Host string `toml:"host"`
		Port int    `toml:"port"`
	} `toml:"mpd"`
	Upload struct {
		Host string `toml:"host"`
		Path string `toml:"path"`
	} `toml:"upload"`
	Output struct {
		TempFile string `toml:"temp_file"`
	} `toml:"output"`
}

type exportAlbum struct {
	Artist  string  `json:"artist"`
	Album   string  `json:"album"`
	Year    string  `json:"year"`
	YearInt int     `json:"year_int"`
	Rating  int     `json:"rating"`
	Computed float64 `json:"computed"`
}

// ---------------------------------------------------------------------------
// MPD client (minimal, just what we need)
// ---------------------------------------------------------------------------

type mpdClient struct {
	conn net.Conn
	r    *bufio.Reader
	w    *bufio.Writer
}

func newMPDClient(host string, port int) (*mpdClient, error) {
	conn, err := net.DialTimeout("tcp", fmt.Sprintf("%s:%d", host, port), 5*time.Second)
	if err != nil {
		return nil, err
	}
	c := &mpdClient{conn: conn, r: bufio.NewReader(conn), w: bufio.NewWriter(conn)}
	line, err := c.r.ReadString('\n')
	if err != nil {
		conn.Close()
		return nil, err
	}
	if !strings.HasPrefix(line, "OK MPD") {
		conn.Close()
		return nil, fmt.Errorf("unexpected greeting: %s", strings.TrimSpace(line))
	}
	return c, nil
}

func (c *mpdClient) cmd(command string) ([]string, error) {
	c.conn.SetDeadline(time.Now().Add(30 * time.Second))
	if _, err := c.w.WriteString(command + "\n"); err != nil {
		return nil, err
	}
	if err := c.w.Flush(); err != nil {
		return nil, err
	}
	var lines []string
	for {
		line, err := c.r.ReadString('\n')
		if err != nil {
			return nil, err
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "OK" {
			return lines, nil
		}
		if strings.HasPrefix(line, "ACK ") {
			return nil, fmt.Errorf("mpd: %s", line)
		}
		lines = append(lines, line)
	}
}

func (c *mpdClient) close() { c.conn.Close() }

func mpdQuote(s string) string {
	return `"` + strings.ReplaceAll(strings.ReplaceAll(s, `\`, `\\`), `"`, `\"`) + `"`
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

func main() {
	start := time.Now()
	logf(start, "Starting...")

	cfg, err := loadConfig()
	if err != nil {
		fatal(start, err)
	}

	logf(start, "Connecting to melodyd at %s:%d...", cfg.MPD.Host, cfg.MPD.Port)
	mpd, err := newMPDClient(cfg.MPD.Host, cfg.MPD.Port)
	if err != nil {
		fatal(start, fmt.Errorf("connect: %w", err))
	}
	defer mpd.close()

	logf(start, "Fetching albums...")
	lines, err := mpd.cmd("list Album group AlbumArtist group Date")
	if err != nil {
		fatal(start, fmt.Errorf("list albums: %w", err))
	}

	type rawAlbum struct {
		AlbumArtist, Album, Date string
	}
	var raws []rawAlbum
	var cur rawAlbum
	for _, l := range lines {
		k, v, ok := strings.Cut(l, ": ")
		if !ok {
			continue
		}
		switch k {
		case "AlbumArtist":
			cur.AlbumArtist = v
		case "Date":
			cur.Date = v
		case "Album":
			cur.Album = v
			raws = append(raws, cur)
		}
	}
	logf(start, "Found %d albums, fetching ratings...", len(raws))

	albums := make([]exportAlbum, 0, len(raws))
	for _, raw := range raws {
		yearInt, _ := strconv.Atoi(raw.Date)
		ea := exportAlbum{
			Artist:  fallback(raw.AlbumArtist, "Unknown Artist"),
			Album:   fallback(raw.Album, "Unknown Album"),
			Year:    fallback(raw.Date, "N/A"),
			YearInt: yearInt,
		}

		ratingLines, err := mpd.cmd("getalbumrating " + mpdQuote(raw.AlbumArtist) + " " + mpdQuote(raw.Album) + " " + mpdQuote(raw.Date))
		if err == nil {
			for _, rl := range ratingLines {
				if strings.HasPrefix(rl, "rating: ") {
					ea.Rating, _ = strconv.Atoi(strings.TrimPrefix(rl, "rating: "))
				}
				if strings.HasPrefix(rl, "computed: ") {
					ea.Computed, _ = strconv.ParseFloat(strings.TrimPrefix(rl, "computed: "), 64)
				}
			}
		}
		albums = append(albums, ea)
	}

	payload, err := json.Marshal(albums)
	if err != nil {
		fatal(start, fmt.Errorf("marshal: %w", err))
	}

	tempFile := cfg.Output.TempFile
	if err := os.WriteFile(tempFile, []byte(renderHTML(string(payload))), 0o644); err != nil {
		fatal(start, fmt.Errorf("write html: %w", err))
	}
	logf(start, "HTML generated (%d albums)", len(albums))

	target := cfg.Upload.Host + ":" + cfg.Upload.Path + "/index.html"
	logf(start, "Uploading to %s...", target)
	cmd := exec.Command("scp", tempFile, target)
	output, err := cmd.CombinedOutput()
	if err != nil {
		_ = os.Remove(tempFile)
		fatal(start, fmt.Errorf("scp failed: %s", strings.TrimSpace(string(output))))
	}
	_ = os.Remove(tempFile)
	logf(start, "Done.")
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

func loadConfig() (config, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return config{}, err
	}
	xdgConfig := os.Getenv("XDG_CONFIG_HOME")
	if xdgConfig == "" {
		xdgConfig = filepath.Join(home, ".config")
	}
	confPath := filepath.Join(xdgConfig, "melody", "melody-musiclist.toml")
	_ = os.MkdirAll(filepath.Dir(confPath), 0o755)

	if _, err := os.Stat(confPath); os.IsNotExist(err) {
		_ = os.WriteFile(confPath, []byte(defaultConfig()), 0o644)
	}

	var cfg config
	if _, err := toml.DecodeFile(confPath, &cfg); err != nil {
		return config{}, err
	}
	if h := os.Getenv("MPD_HOST"); h != "" {
		if host, port, ok := strings.Cut(h, ":"); ok {
			cfg.MPD.Host = host
			fmt.Sscanf(port, "%d", &cfg.MPD.Port)
		} else {
			cfg.MPD.Host = h
		}
	}
	if p := os.Getenv("MPD_PORT"); p != "" {
		fmt.Sscanf(p, "%d", &cfg.MPD.Port)
	}
	if cfg.MPD.Host == "" {
		cfg.MPD.Host = "localhost"
	}
	if cfg.MPD.Port == 0 {
		cfg.MPD.Port = 6600
	}
	if cfg.Upload.Host == "" {
		cfg.Upload.Host = "proteus"
	}
	if cfg.Upload.Path == "" {
		cfg.Upload.Path = "/srv/http/list"
	}
	if cfg.Output.TempFile == "" {
		cfg.Output.TempFile = "/tmp/musiclist.html"
	}
	return cfg, nil
}

func defaultConfig() string {
	return `[mpd]
host = "localhost"
port = 6600

[upload]
host = "proteus"
path = "/srv/http/list"

[output]
temp_file = "/tmp/musiclist.html"
`
}

// ---------------------------------------------------------------------------
// HTML
// ---------------------------------------------------------------------------

func renderHTML(jsonData string) string {
	return fmt.Sprintf(`<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Music Collection</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;500&family=Inter:wght@300;400;500;600;700&display=swap" rel="stylesheet">
  <style>
    :root {
      --bg: #fafaf9; --bg-card: #ffffff; --bg-hover: #f5f5f4;
      --bg-badge: #f5f5f4; --bg-header: #f8f8f7;
      --border: #e7e5e4; --border-focus: #a8a29e;
      --text: #1c1917; --text-secondary: #57534e; --text-muted: #a8a29e;
      --accent: #dc2626; --accent-hover: #b91c1c; --accent-soft: #fef2f2;
      --star: #d6d3d1; --star-filled: #f59e0b; --star-computed: #fbbf24;
      --shadow: 0 1px 3px rgba(0,0,0,0.06), 0 1px 2px rgba(0,0,0,0.04);
      --row-alt: rgba(0,0,0,0.015);
    }
    .dark {
      --bg: #0c0a09; --bg-card: #1c1917; --bg-hover: #292524;
      --bg-badge: #292524; --bg-header: #1a1816;
      --border: #292524; --border-focus: #57534e;
      --text: #fafaf9; --text-secondary: #a8a29e; --text-muted: #57534e;
      --accent: #ef4444; --accent-hover: #f87171; --accent-soft: #1c1917;
      --star: #44403c; --star-filled: #f59e0b; --star-computed: #d97706;
      --shadow: 0 1px 3px rgba(0,0,0,0.3);
      --row-alt: rgba(255,255,255,0.02);
    }
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    html { scroll-behavior: smooth; }
    body {
      font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
      background: var(--bg); color: var(--text);
      line-height: 1.6; min-height: 100vh;
      transition: background 0.3s, color 0.3s;
    }
    .container { max-width: 1200px; margin: 0 auto; padding: 2rem 1.5rem; }

    /* Header */
    .header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 2rem; }
    .header-left h1 {
      font-size: 1.75rem; font-weight: 700; letter-spacing: -0.03em;
      background: linear-gradient(135deg, var(--accent), #f97316);
      -webkit-background-clip: text; -webkit-text-fill-color: transparent;
      background-clip: text;
    }
    .header-left .subtitle {
      font-size: 0.8rem; color: var(--text-muted); margin-top: 0.15rem;
      font-family: 'JetBrains Mono', monospace; letter-spacing: 0.02em;
    }
    .header-right { display: flex; align-items: center; gap: 0.75rem; }
    .stat-badge {
      font-family: 'JetBrains Mono', monospace; font-size: 0.75rem;
      color: var(--text-secondary); background: var(--bg-badge);
      padding: 0.35rem 0.75rem; border-radius: 2rem; border: 1px solid var(--border);
    }
    .theme-toggle {
      width: 36px; height: 36px; border-radius: 50%%; border: 1px solid var(--border);
      background: var(--bg-card); cursor: pointer; display: flex; align-items: center;
      justify-content: center; font-size: 1rem; transition: all 0.2s;
      color: var(--text-secondary);
    }
    .theme-toggle:hover { border-color: var(--accent); color: var(--accent); }

    /* Alphabet filter */
    .alpha-bar {
      display: flex; flex-wrap: wrap; justify-content: center; gap: 0.25rem;
      margin-bottom: 1.25rem;
    }
    .alpha-btn {
      font-family: 'JetBrains Mono', monospace; font-size: 0.7rem; font-weight: 500;
      width: 30px; height: 30px; display: flex; align-items: center; justify-content: center;
      border-radius: 6px; border: 1px solid var(--border); background: var(--bg-card);
      color: var(--text-secondary); cursor: pointer; transition: all 0.15s;
    }
    .alpha-btn:hover { border-color: var(--accent); color: var(--accent); }
    .alpha-btn.active { background: var(--accent); color: #fff; border-color: var(--accent); }

    /* Search */
    .search-bar { position: relative; margin-bottom: 1.25rem; }
    .search-bar input {
      width: 100%%; font-size: 0.9rem; padding: 0.75rem 1rem 0.75rem 2.75rem;
      border: 1px solid var(--border); border-radius: 12px; background: var(--bg-card);
      color: var(--text); outline: none; transition: all 0.2s; box-shadow: var(--shadow);
      font-family: inherit;
    }
    .search-bar input:focus { border-color: var(--accent); box-shadow: 0 0 0 3px var(--accent-soft); }
    .search-bar input::placeholder { color: var(--text-muted); }
    .search-bar .search-icon {
      position: absolute; left: 0.9rem; top: 50%%; transform: translateY(-50%%);
      color: var(--text-muted); font-size: 1rem; pointer-events: none;
    }
    .search-bar .clear-btn {
      position: absolute; right: 0.75rem; top: 50%%; transform: translateY(-50%%);
      background: none; border: none; color: var(--text-muted); cursor: pointer;
      font-size: 1.1rem; padding: 0.25rem; border-radius: 4px; display: none; line-height: 1;
    }
    .search-bar .clear-btn:hover { color: var(--accent); }
    .search-bar.has-value .clear-btn { display: block; }

    /* Table */
    .table-wrap {
      background: var(--bg-card); border: 1px solid var(--border);
      border-radius: 12px; overflow: hidden; box-shadow: var(--shadow);
      margin-bottom: 1.5rem;
    }
    .table-wrap table { width: 100%%; border-collapse: collapse; }
    .table-wrap th {
      text-align: left; padding: 0.7rem 1rem; font-size: 0.7rem; font-weight: 600;
      text-transform: uppercase; letter-spacing: 0.05em; color: var(--text-muted);
      background: var(--bg-header); border-bottom: 1px solid var(--border);
      cursor: pointer; user-select: none; transition: color 0.15s;
      white-space: nowrap;
    }
    .table-wrap th:hover { color: var(--accent); }
    .table-wrap th.sorted { color: var(--accent); }
    .table-wrap th .arrow { font-size: 0.6rem; margin-left: 0.3rem; opacity: 0.5; }
    .table-wrap th.sorted .arrow { opacity: 1; }
    .table-wrap td { padding: 0.55rem 1rem; font-size: 0.85rem; border-bottom: 1px solid var(--border); }
    .table-wrap tr:last-child td { border-bottom: none; }
    .table-wrap tbody tr { transition: background 0.1s; }
    .table-wrap tbody tr:nth-child(even) { background: var(--row-alt); }
    .table-wrap tbody tr:hover { background: var(--bg-hover); }
    .col-artist { font-weight: 500; color: var(--text); max-width: 300px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .col-album { color: var(--text-secondary); max-width: 350px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .col-year { font-family: 'JetBrains Mono', monospace; font-size: 0.8rem; color: var(--text-muted); width: 5rem; }
    .col-rating { width: 7rem; white-space: nowrap; }
    .col-rating .s { color: var(--star); }
    .col-rating .s.on { color: var(--star-filled); }
    .col-rating .s.computed { color: var(--star-computed); opacity: 0.6; }
    .col-rating .no-rating { font-size: 0.75rem; color: var(--text-muted); }

    /* Empty state */
    .empty-row td { text-align: center; padding: 3rem 1rem; color: var(--text-muted); font-size: 0.9rem; }

    /* Pagination */
    .pagination {
      display: flex; align-items: center; justify-content: space-between;
      flex-wrap: wrap; gap: 1rem;
    }
    .pagination .info {
      font-size: 0.75rem; color: var(--text-muted);
      font-family: 'JetBrains Mono', monospace;
    }
    .pagination .controls { display: flex; align-items: center; gap: 0.5rem; }
    .pagination select {
      font-size: 0.75rem; padding: 0.35rem 0.5rem; border-radius: 6px;
      border: 1px solid var(--border); background: var(--bg-card); color: var(--text);
      cursor: pointer; font-family: 'JetBrains Mono', monospace;
    }
    .page-btn {
      font-size: 0.75rem; padding: 0.35rem 0.75rem; border-radius: 6px;
      border: 1px solid var(--border); background: var(--bg-card); color: var(--text-secondary);
      cursor: pointer; transition: all 0.15s; font-family: inherit;
    }
    .page-btn:hover:not(:disabled) { border-color: var(--accent); color: var(--accent); }
    .page-btn:disabled { opacity: 0.35; cursor: not-allowed; }
    .page-indicator {
      font-family: 'JetBrains Mono', monospace; font-size: 0.75rem;
      color: var(--text-secondary); min-width: 7rem; text-align: center;
    }

    @media (max-width: 640px) {
      .container { padding: 1rem; }
      .header-left h1 { font-size: 1.3rem; }
      .table-wrap td, .table-wrap th { padding: 0.5rem 0.6rem; }
      .col-artist, .col-album { max-width: 150px; }
      .pagination { flex-direction: column; align-items: stretch; text-align: center; }
      .pagination .controls { justify-content: center; }
    }
  </style>
</head>
<body>
<div class="container">
  <div class="header">
    <div class="header-left">
      <h1>Music Collection</h1>
      <div class="subtitle">generated %s</div>
    </div>
    <div class="header-right">
      <span class="stat-badge" id="album-count"></span>
      <button class="theme-toggle" id="darkModeToggle" title="Toggle theme">
        <span class="toggle-icon"></span>
      </button>
    </div>
  </div>

  <div class="alpha-bar" id="alphabet-filter-buttons"></div>

  <div class="search-bar" id="search-bar">
    <span class="search-icon">&#x2315;</span>
    <input type="text" id="filter" placeholder="Search artists, albums, years... or r=N for rating">
    <button class="clear-btn" id="clearFilter">&#x2715;</button>
  </div>

  <div class="table-wrap">
    <table>
      <thead>
        <tr>
          <th class="sorted" data-sort="artist">Artist <span class="arrow">&#9650;</span></th>
          <th data-sort="album">Album <span class="arrow"></span></th>
          <th data-sort="year">Year <span class="arrow"></span></th>
          <th data-sort="rating">Rating <span class="arrow"></span></th>
        </tr>
      </thead>
      <tbody id="album-tbody"></tbody>
    </table>
  </div>

  <div class="pagination">
    <div class="info"><span id="pagination-info"></span></div>
    <div class="controls">
      <select id="itemsPerPage">
        <option value="25">25</option>
        <option value="50" selected>50</option>
        <option value="100">100</option>
        <option value="250">250</option>
      </select>
      <button class="page-btn" id="prev-page">&#8592; Prev</button>
      <span class="page-indicator" id="page-indicator"></span>
      <button class="page-btn" id="next-page">Next &#8594;</button>
    </div>
  </div>
</div>

<script>
const allAlbums = %s;
const SYM = "#SYM#";
let page = 1, perPage = 50, sortCol = 'artist', sortDir = 'asc';
let textFilter = '', letterFilter = null, filtered = [];

const $ = id => document.getElementById(id);
const tbody = $('album-tbody'), filterInput = $('filter'), clearBtn = $('clearFilter');
const prevBtn = $('prev-page'), nextBtn = $('next-page');
const pageInd = $('page-indicator'), pageInfo = $('pagination-info');
const perPageSel = $('itemsPerPage'), alphaBar = $('alphabet-filter-buttons');
const searchBar = $('search-bar'), countBadge = $('album-count');
const headers = document.querySelectorAll('.table-wrap th[data-sort]');

// Theme
try {
  const t = localStorage.theme, p = matchMedia('(prefers-color-scheme:dark)').matches;
  if (t === 'dark' || (!t && p)) document.documentElement.classList.add('dark');
} catch(_){}
function updateThemeIcon() {
  const d = document.documentElement.classList.contains('dark');
  document.querySelector('.toggle-icon').textContent = d ? '\u2600' : '\u263E';
}
updateThemeIcon();
$('darkModeToggle').onclick = () => {
  const d = document.documentElement.classList.toggle('dark');
  try { localStorage.theme = d ? 'dark' : 'light'; } catch(_){}
  updateThemeIcon();
};

function norm(s) { return s ? s.normalize("NFD").replace(/\p{Diacritic}/gu,"").toUpperCase() : ""; }
function esc(s) { const d = document.createElement('div'); d.textContent = s; return d.innerHTML; }

function stars(rating, computed) {
  const display = rating > 0 ? rating : (computed > 0 ? Math.round(computed) : 0);
  const isComp = rating === 0 && computed > 0;
  if (!display) return '<span class="no-rating">\u2014</span>';
  let h = '';
  for (let i = 1; i <= 5; i++)
    h += '<span class="s' + (i <= display ? (isComp ? ' computed' : ' on') : '') + '">\u2605</span>';
  if (isComp) h = '<span title="Avg: ' + computed.toFixed(1) + '">' + h + '</span>';
  return h;
}

// Alphabet
function buildAlpha() {
  const letters = new Set(); let sym = false;
  allAlbums.forEach(a => {
    const f = norm((a.artist||'')[0]||'');
    if (!f) return;
    if (f >= 'A' && f <= 'Z') letters.add(f); else sym = true;
  });
  alphaBar.innerHTML = '';
  const all = ['All', ...'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.split('').filter(l=>letters.has(l)), ...(sym?['#']:[])];
  all.forEach(l => {
    const b = document.createElement('button');
    b.textContent = l; b.className = 'alpha-btn';
    b.dataset.letter = l === '#' ? SYM : l === 'All' ? 'ALL' : l;
    b.onclick = () => {
      const k = b.dataset.letter;
      letterFilter = (k === 'ALL' || letterFilter === k) ? null : k;
      styleAlpha(); run();
    };
    alphaBar.appendChild(b);
  });
  styleAlpha();
}
function styleAlpha() {
  alphaBar.querySelectorAll('.alpha-btn').forEach(b => {
    b.classList.toggle('active', (letterFilter===null && b.dataset.letter==='ALL') || b.dataset.letter===letterFilter);
  });
}

function run() {
  let rFilter = -1;
  const raw = textFilter.toLowerCase().split(' ').filter(Boolean), terms = [];
  raw.forEach(t => {
    if (t.startsWith('r=')) { const n = parseInt(t.slice(2),10); if(!isNaN(n)&&n>=0&&n<=5) rFilter=n; else terms.push(t); }
    else terms.push(t);
  });
  let a = [...allAlbums];
  if (letterFilter) a = a.filter(al => {
    const f = norm((al.artist||'')[0]||'');
    return letterFilter === SYM ? !(f>='A'&&f<='Z') : f === letterFilter;
  });
  if (rFilter !== -1) a = a.filter(al => al.rating === rFilter);
  if (terms.length) a = a.filter(al => {
    const s = (al.artist+' '+al.album+' '+al.year).toLowerCase();
    return terms.every(t => s.includes(t));
  });
  a.sort((x,y) => {
    let xv, yv;
    if (sortCol==='year') { xv=x.year_int; yv=y.year_int; }
    else if (sortCol==='rating') { xv=x.rating||x.computed||0; yv=y.rating||y.computed||0; }
    else { xv=norm(x[sortCol]); yv=norm(y[sortCol]); }
    let c = xv>yv?1:xv<yv?-1:0;
    return sortDir==='desc'?-c:c;
  });
  filtered = a; page = 1;
  render(); paginate();
}

function render() {
  tbody.innerHTML = '';
  countBadge.textContent = filtered.length + ' albums';
  if (!filtered.length) {
    tbody.innerHTML = '<tr class="empty-row"><td colspan="4">\u266A No albums match your search.</td></tr>';
    return;
  }
  const start = (page-1)*perPage, end = start+perPage;
  filtered.slice(start, end).forEach(a => {
    const tr = document.createElement('tr');
    tr.innerHTML =
      '<td class="col-artist">' + esc(a.artist) + '</td>' +
      '<td class="col-album">' + esc(a.album) + '</td>' +
      '<td class="col-year">' + esc(a.year) + '</td>' +
      '<td class="col-rating">' + stars(a.rating, a.computed) + '</td>';
    tbody.appendChild(tr);
  });
}

function paginate() {
  const total = filtered.length, pages = Math.ceil(total/perPage)||1;
  pageInd.textContent = page + ' / ' + pages;
  const s = total ? (page-1)*perPage+1 : 0, e = Math.min(page*perPage, total);
  pageInfo.textContent = s + '\u2013' + e + ' of ' + total;
  prevBtn.disabled = page === 1;
  nextBtn.disabled = page === pages;
}

// Sort headers
headers.forEach(th => th.onclick = () => {
  const col = th.dataset.sort;
  if (sortCol === col) sortDir = sortDir==='asc'?'desc':'asc';
  else { sortCol = col; sortDir = 'asc'; }
  headers.forEach(h => { h.classList.remove('sorted'); h.querySelector('.arrow').textContent = ''; });
  th.classList.add('sorted');
  th.querySelector('.arrow').textContent = sortDir==='asc' ? '\u25B2' : '\u25BC';
  run();
});

filterInput.oninput = () => {
  textFilter = filterInput.value;
  searchBar.classList.toggle('has-value', !!textFilter);
  run();
};
clearBtn.onclick = () => {
  filterInput.value = ''; textFilter = ''; letterFilter = null;
  searchBar.classList.remove('has-value');
  styleAlpha(); run();
};
prevBtn.onclick = () => { if(page>1){page--;render();paginate();} };
nextBtn.onclick = () => { if(page<Math.ceil(filtered.length/perPage)){page++;render();paginate();} };
perPageSel.onchange = e => { perPage=parseInt(e.target.value,10); page=1; render(); paginate(); };

buildAlpha();
run();
</script>
</body>
</html>`, time.Now().Format("2006-01-02 15:04"), jsonData)
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func fallback(value, alt string) string {
	if strings.TrimSpace(value) == "" {
		return alt
	}
	return value
}

func logf(start time.Time, format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("[%s | %.3fs] %s\n", time.Now().Format("15:04:05.000"), time.Since(start).Seconds(), msg)
}

func fatal(start time.Time, err error) {
	logf(start, "Error: %v", err)
	os.Exit(1)
}
