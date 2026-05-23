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
  <title>Music Library</title>
  <script src="https://cdn.tailwindcss.com/3.4.3"></script>
  <script>tailwind.config = { darkMode: 'class' }</script>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
  <style>
    body { font-family: 'Inter', sans-serif; }
    th.sortable { cursor: pointer; user-select: none; }
    html:not(.dark) th.sortable:hover { background-color: #f0f4f8; }
    html.dark th.sortable:hover { background-color: #374151; }
    th .sort-icon { display: inline-block; margin-left: 5px; opacity: 0.4; width: 1em; vertical-align: middle; }
    th.sorted-asc .sort-icon::after { content: ' ▲'; opacity: 1; }
    th.sorted-desc .sort-icon::after { content: ' ▼'; opacity: 1; }
    .star { color: #d1d5db; }
    .star.filled { color: #eab308; }
    html.dark .star { color: #4b5563; }
    html.dark .star.filled { color: #eab308; }
    .alpha-button.active { background-color: #4f46e5; color: white; }
    html.dark .alpha-button.active { background-color: #6366f1; }
  </style>
</head>
<body class="bg-gray-100 dark:bg-gray-900 text-gray-800 dark:text-gray-200">
<div class="container mx-auto px-4 py-8 max-w-7xl">
  <div class="flex justify-between items-center mb-2">
    <h1 class="text-3xl font-bold text-gray-700 dark:text-gray-300">Music Library</h1>
    <button id="darkModeToggle" title="Toggle Dark Mode" class="p-2 rounded-full text-gray-500 dark:text-gray-400 hover:bg-gray-200 dark:hover:bg-gray-700">
      <span class="sun-icon">☀</span><span class="moon-icon">☾</span>
    </button>
  </div>
  <p class="text-sm text-gray-500 dark:text-gray-400 text-center mb-6">Generated on %s</p>
  <div id="alphabet-filter-buttons" class="mb-6 flex flex-wrap justify-center gap-1 sm:gap-2"></div>
  <div class="mb-6 bg-white dark:bg-gray-800 p-4 rounded-lg shadow-sm flex flex-wrap gap-4 items-end">
    <div class="flex-grow min-w-[200px]">
      <label for="filter" class="block text-sm font-medium text-gray-700 dark:text-gray-300 mb-1">Filter:</label>
      <input type="text" id="filter" placeholder="Artist, album, year, or r=N..." class="w-full px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md shadow-sm bg-white dark:bg-gray-700 text-gray-900 dark:text-gray-100">
    </div>
    <button id="clearFilter" class="px-4 py-2 bg-gray-200 dark:bg-gray-600 text-gray-700 dark:text-gray-200 rounded-md hover:bg-gray-300 dark:hover:bg-gray-500 text-sm">Clear</button>
  </div>
  <div class="bg-white dark:bg-gray-800 rounded-lg shadow overflow-hidden">
    <div class="table-container overflow-x-auto">
      <table class="min-w-full divide-y divide-gray-200 dark:divide-gray-700">
        <thead class="bg-gray-50 dark:bg-gray-700">
          <tr>
            <th scope="col" data-sort="artist" class="sortable px-6 py-3 text-left text-xs font-medium text-gray-500 dark:text-gray-400 uppercase tracking-wider">Artist <span class="sort-icon"></span></th>
            <th scope="col" data-sort="album" class="sortable px-6 py-3 text-left text-xs font-medium text-gray-500 dark:text-gray-400 uppercase tracking-wider">Album <span class="sort-icon"></span></th>
            <th scope="col" data-sort="year" class="sortable px-6 py-3 text-left text-xs font-medium text-gray-500 dark:text-gray-400 uppercase tracking-wider w-28">Year <span class="sort-icon"></span></th>
            <th scope="col" data-sort="rating" class="sortable px-6 py-3 text-left text-xs font-medium text-gray-500 dark:text-gray-400 uppercase tracking-wider w-32">Rating <span class="sort-icon"></span></th>
          </tr>
        </thead>
        <tbody id="album-table-body" class="bg-white dark:bg-gray-800 divide-y divide-gray-200 dark:divide-gray-700"></tbody>
      </table>
    </div>
  </div>
  <div class="mt-6 flex flex-col sm:flex-row justify-between items-center text-sm text-gray-600 dark:text-gray-400">
    <div class="mb-2 sm:mb-0"><span id="pagination-info">Showing 0 to 0 of 0 entries</span></div>
    <div class="flex items-center space-x-1">
      <label for="itemsPerPage" class="mr-2 font-medium text-gray-700 dark:text-gray-300">Per Page:</label>
      <select id="itemsPerPage" class="border border-gray-300 dark:border-gray-600 rounded-md px-2 py-1 bg-white dark:bg-gray-700 text-gray-900 dark:text-gray-100">
        <option value="25">25</option>
        <option value="50" selected>50</option>
        <option value="100">100</option>
        <option value="250">250</option>
      </select>
      <button id="prev-page" class="px-3 py-1 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-700">Previous</button>
      <span id="page-indicator" class="px-3 py-1">Page 1 of 1</span>
      <button id="next-page" class="px-3 py-1 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-700">Next</button>
    </div>
  </div>
</div>
<script>
  const allAlbums = %s;
  const SYMBOL_FILTER_KEY = "#SYMBOLS#";
  let currentPage = 1;
  let itemsPerPage = 50;
  let currentSortColumn = 'artist';
  let currentSortDirection = 'asc';
  let currentTextFilter = '';
  let currentLetterFilter = null;
  let filteredAndSortedAlbums = [];
  const tableBody = document.getElementById('album-table-body');
  const filterInput = document.getElementById('filter');
  const clearFilterButton = document.getElementById('clearFilter');
  const prevButton = document.getElementById('prev-page');
  const nextButton = document.getElementById('next-page');
  const pageIndicator = document.getElementById('page-indicator');
  const paginationInfo = document.getElementById('pagination-info');
  const itemsPerPageSelect = document.getElementById('itemsPerPage');
  const tableHeaders = document.querySelectorAll('th.sortable');
  const darkModeToggle = document.getElementById('darkModeToggle');
  const alphabetButtonsContainer = document.getElementById('alphabet-filter-buttons');
  try {
    const theme = localStorage.theme;
    const prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
    if (theme === 'dark' || (!theme && prefersDark)) document.documentElement.classList.add('dark');
  } catch (_) {}
  function normalizeForSearch(str) {
    if (!str) return '';
    return str.normalize("NFD").replace(/\p{Diacritic}/gu, "").toUpperCase();
  }
  function renderStars(rating) {
    let html = '';
    for (let i = 1; i <= 5; i++) {
      html += '<span class="star' + (i <= rating ? ' filled' : '') + '">★</span>';
    }
    return html;
  }
  function generateAlphabetButtons() {
    const availableLetters = new Set();
    let hasSymbolChars = false;
    allAlbums.forEach(album => {
      if (!album.artist) return;
      const first = normalizeForSearch(album.artist[0]);
      if (!first) return;
      if (first >= 'A' && first <= 'Z') availableLetters.add(first); else hasSymbolChars = true;
    });
    alphabetButtonsContainer.innerHTML = '';
    ['All', ...'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.split('').filter(letter => availableLetters.has(letter)), ...(hasSymbolChars ? ['#'] : [])].forEach(letter => {
      const button = document.createElement('button');
      button.textContent = letter;
      button.dataset.letter = letter === '#' ? SYMBOL_FILTER_KEY : (letter === 'All' ? 'ALL' : letter);
      button.className = 'alpha-button px-3 py-1 sm:px-2 sm:py-1 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-700 text-gray-700 dark:text-gray-300 hover:bg-gray-100 dark:hover:bg-gray-600 text-sm font-medium';
      button.addEventListener('click', () => handleAlphabetFilterClick(button.dataset.letter));
      alphabetButtonsContainer.appendChild(button);
    });
    updateAlphabetButtonStyles();
  }
  function updateAlphabetButtonStyles() {
    alphabetButtonsContainer.querySelectorAll('.alpha-button').forEach(button => {
      button.classList.toggle('active', (currentLetterFilter === null && button.dataset.letter === 'ALL') || button.dataset.letter === currentLetterFilter);
    });
  }
  function handleAlphabetFilterClick(letterOrKey) {
    if (letterOrKey === 'ALL' || currentLetterFilter === letterOrKey) currentLetterFilter = null; else currentLetterFilter = letterOrKey;
    updateAlphabetButtonStyles();
    applyFilterAndSort();
  }
  function applyFilterAndSort() {
    let ratingFilterValue = -1;
    const rawTerms = currentTextFilter.toLowerCase().split(' ').filter(Boolean);
    const textTerms = [];
    rawTerms.forEach(term => {
      if (term.startsWith('r=')) {
        const n = parseInt(term.slice(2), 10);
        if (!isNaN(n) && n >= 0 && n <= 5) ratingFilterValue = n; else textTerms.push(term);
      } else textTerms.push(term);
    });
    let albums = [...allAlbums];
    if (currentLetterFilter) {
      albums = albums.filter(album => {
        const first = normalizeForSearch((album.artist || '')[0] || '');
        if (currentLetterFilter === SYMBOL_FILTER_KEY) return !(first >= 'A' && first <= 'Z');
        return first === currentLetterFilter;
      });
    }
    if (ratingFilterValue !== -1) albums = albums.filter(album => album.rating === ratingFilterValue);
    if (textTerms.length) albums = albums.filter(album => {
      const searchableText = (album.artist + ' ' + album.album + ' ' + album.year).toLowerCase();
      return textTerms.every(term => searchableText.includes(term));
    });
    albums.sort((a, b) => {
      let av, bv;
      if (currentSortColumn === 'year') { av = a.year_int; bv = b.year_int; }
      else if (currentSortColumn === 'rating') { av = a.rating || a.computed || 0; bv = b.rating || b.computed || 0; }
      else { av = normalizeForSearch(a[currentSortColumn]); bv = normalizeForSearch(b[currentSortColumn]); }
      let cmp = av > bv ? 1 : av < bv ? -1 : 0;
      return currentSortDirection === 'desc' ? -cmp : cmp;
    });
    filteredAndSortedAlbums = albums;
    currentPage = 1;
    updateTable();
    updatePaginationControls();
  }
  function updateTable() {
    tableBody.innerHTML = '';
    if (!filteredAndSortedAlbums.length) {
      tableBody.innerHTML = '<tr><td colspan="4" class="text-center py-10 text-gray-500 dark:text-gray-400">No albums match your criteria.</td></tr>';
      return;
    }
    const start = (currentPage - 1) * itemsPerPage;
    const end = start + itemsPerPage;
    filteredAndSortedAlbums.slice(start, end).forEach(album => {
      const row = document.createElement('tr');
      row.className = 'hover:bg-gray-50 dark:hover:bg-gray-700';
      const displayRating = album.rating > 0 ? album.rating : (album.computed > 0 ? Math.round(album.computed) : 0);
      const isComputed = album.rating === 0 && album.computed > 0;
      const starsHtml = displayRating > 0
        ? '<span' + (isComputed ? ' title="Avg of track ratings: ' + album.computed.toFixed(1) + '" style="opacity:0.6"' : '') + '>' + renderStars(displayRating) + '</span>'
        : '<span class="text-gray-400 dark:text-gray-600 text-xs">—</span>';
      row.innerHTML =
        '<td class="px-6 py-4 whitespace-nowrap text-sm font-medium text-gray-900 dark:text-gray-100">' + album.artist + '</td>' +
        '<td class="px-6 py-4 whitespace-nowrap text-sm text-gray-600 dark:text-gray-300">' + album.album + '</td>' +
        '<td class="px-6 py-4 whitespace-nowrap text-sm text-gray-600 dark:text-gray-300">' + album.year + '</td>' +
        '<td class="px-6 py-4 whitespace-nowrap text-sm">' + starsHtml + '</td>';
      tableBody.appendChild(row);
    });
  }
  function updatePaginationControls() {
    const totalItems = filteredAndSortedAlbums.length;
    const totalPages = Math.ceil(totalItems / itemsPerPage) || 1;
    pageIndicator.textContent = 'Page ' + currentPage + ' of ' + totalPages;
    const startItem = totalItems === 0 ? 0 : (currentPage - 1) * itemsPerPage + 1;
    const endItem = Math.min(currentPage * itemsPerPage, totalItems);
    paginationInfo.textContent = 'Showing ' + startItem + ' to ' + endItem + ' of ' + totalItems + ' entries';
    prevButton.disabled = currentPage === 1;
    nextButton.disabled = currentPage === totalPages;
  }
  tableHeaders.forEach(header => header.addEventListener('click', () => {
    const sortKey = header.dataset.sort;
    if (currentSortColumn === sortKey) currentSortDirection = currentSortDirection === 'asc' ? 'desc' : 'asc'; else { currentSortColumn = sortKey; currentSortDirection = 'asc'; }
    applyFilterAndSort();
    tableHeaders.forEach(h => h.classList.remove('sorted-asc', 'sorted-desc'));
    header.classList.add(currentSortDirection === 'asc' ? 'sorted-asc' : 'sorted-desc');
  }));
  filterInput.addEventListener('input', () => { currentTextFilter = filterInput.value; applyFilterAndSort(); });
  clearFilterButton.addEventListener('click', () => { filterInput.value = ''; currentTextFilter = ''; currentLetterFilter = null; updateAlphabetButtonStyles(); applyFilterAndSort(); });
  prevButton.addEventListener('click', () => { if (currentPage > 1) { currentPage--; updateTable(); updatePaginationControls(); } });
  nextButton.addEventListener('click', () => { const totalPages = Math.ceil(filteredAndSortedAlbums.length / itemsPerPage); if (currentPage < totalPages) { currentPage++; updateTable(); updatePaginationControls(); } });
  itemsPerPageSelect.addEventListener('change', e => { itemsPerPage = parseInt(e.target.value, 10); currentPage = 1; updateTable(); updatePaginationControls(); });
  if (darkModeToggle) darkModeToggle.addEventListener('click', () => { const dark = document.documentElement.classList.toggle('dark'); try { localStorage.theme = dark ? 'dark' : 'light'; } catch (_) {} });
  generateAlphabetButtons();
  applyFilterAndSort();
</script>
</body>
</html>`, time.Now().Format("2006-01-02 15:04:05"), jsonData)
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
