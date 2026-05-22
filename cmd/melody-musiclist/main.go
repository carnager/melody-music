package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/BurntSushi/toml"
	"github.com/carnager/melody/internal/shared"
)

type config struct {
	API struct {
		Address string `toml:"address"`
		Secret  string `toml:"secret"`
	} `toml:"api"`
	Upload struct {
		Host string `toml:"host"`
		Path string `toml:"path"`
	} `toml:"upload"`
	Output struct {
		TempFile string `toml:"temp_file"`
	} `toml:"output"`
}

type exportAlbum struct {
	ID      string `json:"id"`
	Artist  string `json:"artist"`
	Album   string `json:"album"`
	Year    string `json:"year"`
	YearInt int    `json:"year_int"`
	Rating  int    `json:"rating"`
}

type apiAlbum struct {
	ID          string `json:"id"`
	AlbumArtist string `json:"albumartist"`
	Album       string `json:"album"`
	Date        string `json:"date"`
	Rating      any    `json:"rating"`
}

func main() {
	start := time.Now()
	logf(start, "Script started. Initializing configuration...")

	cfg, err := loadConfig()
	if err != nil {
		fatal(start, err)
	}

	syncTargetHTML := filepath.Join(cfg.Upload.Host+":"+cfg.Upload.Path, "index.html")
	tempHTMLFile := cfg.Output.TempFile

	logf(start, "Loading albums from Melody API...")
	albumsRaw, err := fetchAlbums(cfg.API.Address, cfg.API.Secret)
	if err != nil {
		fatal(start, fmt.Errorf("fetch albums: %w", err))
	}

	albums := make([]exportAlbum, 0, len(albumsRaw))
	for _, raw := range albumsRaw {
		year := stringify(raw["date"])
		yearInt, _ := strconv.Atoi(year)
		albums = append(albums, exportAlbum{
			ID:      stringify(raw["id"]),
			Artist:  fallback(stringify(raw["albumartist"]), "Unknown Artist"),
			Album:   fallback(stringify(raw["album"]), "Unknown Album"),
			Year:    fallback(year, "N/A"),
			YearInt: yearInt,
			Rating:  ratingInt(raw["rating"]),
		})
	}

	payload, err := json.Marshal(albums)
	if err != nil {
		fatal(start, fmt.Errorf("marshal album payload: %w", err))
	}
	if err := os.WriteFile(tempHTMLFile, []byte(renderHTML(string(payload))), 0o644); err != nil {
		fatal(start, fmt.Errorf("write html: %w", err))
	}
	logf(start, "HTML generation complete.")

	logf(start, "Syncing HTML to %s...", syncTargetHTML)
	cmd := exec.Command("scp", tempHTMLFile, syncTargetHTML)
	output, err := cmd.CombinedOutput()
	if err != nil {
		_ = os.Remove(tempHTMLFile)
		fatal(start, fmt.Errorf("scp failed: %s", strings.TrimSpace(string(output))))
	}
	_ = os.Remove(tempHTMLFile)
	logf(start, "Script finished successfully.")
}

func loadConfig() (config, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return config{}, err
	}
	xdgConfig := getenv("XDG_CONFIG_HOME", filepath.Join(home, ".config"))
	confDir := filepath.Join(xdgConfig, "melody")
	confPath := filepath.Join(confDir, "melody-musiclist.toml")
	if err := os.MkdirAll(confDir, 0o755); err != nil {
		return config{}, err
	}
	if _, err := os.Stat(confPath); os.IsNotExist(err) {
		if err := os.WriteFile(confPath, []byte(defaultConfigText()), 0o644); err != nil {
			return config{}, err
		}
	}

	var raw map[string]any
	if _, err := toml.DecodeFile(confPath, &raw); err != nil {
		return config{}, err
	}
	var cfg config
	api, _ := raw["api"].(map[string]any)
	upload, _ := raw["upload"].(map[string]any)
	output, _ := raw["output"].(map[string]any)
	cfg.API.Address = stringify(api["address"])
	cfg.Upload.Host = stringify(upload["host"])
	cfg.Upload.Path = stringify(upload["path"])
	cfg.Output.TempFile = stringify(output["temp_file"])
	applyDefaults(&cfg)
	return cfg, nil
}

func defaultConfigText() string {
	return `[api]
address = "local"

[upload]
host = "proteus"
path = "/srv/http/list"

[output]
temp_file = "/tmp/musiclist_albums_only.html"
`
}

func applyDefaults(cfg *config) {
	if cfg.API.Address == "" {
		cfg.API.Address = shared.LocalAPIConfigValue
	}
	if cfg.Upload.Host == "" {
		cfg.Upload.Host = "proteus"
	}
	if cfg.Upload.Path == "" {
		cfg.Upload.Path = "/srv/http/list"
	}
	if cfg.Output.TempFile == "" {
		cfg.Output.TempFile = "/tmp/musiclist_albums_only.html"
	}
}

func fetchAlbums(address, secret string) ([]map[string]any, error) {
	baseURL, useLocalSocket, socketPath, err := shared.APIBaseURLFromAddress(address)
	if err != nil {
		return nil, err
	}
	client := &http.Client{Timeout: 30 * time.Second}
	if useLocalSocket {
		client = shared.NewLocalHTTPClient(30*time.Second, socketPath)
	}

	req, err := http.NewRequest("GET", baseURL+"/albums", nil)
	if err != nil {
		return nil, err
	}
	if secret != "" {
		req.Header.Set("Authorization", "Bearer "+secret)
	}
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("http %d from %s/albums: %s", resp.StatusCode, baseURL, strings.TrimSpace(string(body)))
	}

	var albums []apiAlbum
	if err := json.NewDecoder(resp.Body).Decode(&albums); err != nil {
		return nil, err
	}

	out := make([]map[string]any, 0, len(albums))
	for _, album := range albums {
		out = append(out, map[string]any{
			"id":          album.ID,
			"albumartist": album.AlbumArtist,
			"album":       album.Album,
			"date":        album.Date,
			"rating":      album.Rating,
		})
	}
	return out, nil
}

func ratingInt(value any) int {
	rating, _ := strconv.Atoi(strings.TrimSpace(stringify(value)))
	if rating < 0 || rating > 10 {
		return 0
	}
	return rating
}

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
    .progress-bar-text { font-size: 0.75rem; line-height: 1rem; font-weight: 600; color: #ffffff; padding: 0 0.25rem; text-shadow: 0px 0px 2px rgba(0, 0, 0, 0.7); }
    #darkModeToggle .sun-icon, #darkModeToggle .moon-icon { display: none; }
    html:not(.dark) #darkModeToggle .sun-icon { display: inline-block; }
    html.dark #darkModeToggle .moon-icon { display: inline-block; }
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
  function getRatingColor(rating) {
    if (rating === 0) return 'bg-gray-400 dark:bg-gray-600';
    if (rating < 4) return 'bg-red-500 dark:bg-red-600';
    if (rating < 7) return 'bg-yellow-500 dark:bg-yellow-600';
    return 'bg-green-500 dark:bg-green-600';
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
        if (!isNaN(n) && n >= 0 && n <= 10) ratingFilterValue = n; else textTerms.push(term);
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
      if (currentSortColumn === 'year' || currentSortColumn === 'rating') {
        av = a[currentSortColumn === 'year' ? 'year_int' : 'rating'];
        bv = b[currentSortColumn === 'year' ? 'year_int' : 'rating'];
      } else {
        av = normalizeForSearch(a[currentSortColumn]);
        bv = normalizeForSearch(b[currentSortColumn]);
      }
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
      const label = album.rating > 0 ? '<span class="absolute inset-0 flex items-center justify-center progress-bar-text">' + album.rating + '/10</span>' : '';
      row.innerHTML =
        '<td class="px-6 py-4 whitespace-nowrap text-sm font-medium text-gray-900 dark:text-gray-100">' + album.artist + '</td>' +
        '<td class="px-6 py-4 whitespace-nowrap text-sm text-gray-600 dark:text-gray-300">' + album.album + '</td>' +
        '<td class="px-6 py-4 whitespace-nowrap text-sm text-gray-600 dark:text-gray-300">' + album.year + '</td>' +
        '<td class="px-6 py-4 whitespace-nowrap text-sm text-gray-600 dark:text-gray-300">' +
          '<div class="w-full bg-gray-200 dark:bg-gray-600 rounded-full h-4 overflow-hidden relative align-middle">' +
            '<div class="' + getRatingColor(album.rating) + ' h-4 rounded-full" style="width: ' + (album.rating * 10) + '%%"></div>' +
            label +
          '</div>' +
        '</td>';
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

func stringify(value any) string {
	return shared.Stringify(value)
}

func fallback(value, alt string) string {
	return shared.Fallback(value, alt)
}

func getenv(key, fallback string) string {
	return shared.Getenv(key, fallback)
}

func logf(start time.Time, format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("[%s | Total: %7.3fs] %s\n", time.Now().Format("15:04:05.000"), time.Since(start).Seconds(), msg)
}

func fatal(start time.Time, err error) {
	logf(start, "Error: %v", err)
	os.Exit(1)
}
