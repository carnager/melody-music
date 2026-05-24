// ---------------------------------------------------------------------------
// Melody Web UI
// ---------------------------------------------------------------------------

(function () {
  "use strict";

  // -------------------------------------------------------------------------
  // Config
  // -------------------------------------------------------------------------

  const RECONNECT_BASE = 1000;
  const RECONNECT_MAX = 30000;

  // -------------------------------------------------------------------------
  // MPD WebSocket Client
  // -------------------------------------------------------------------------

  function createMPD() {
    let cmdWs = null;
    let idleWs = null;
    let cmdQueue = [];
    let reconnectDelay = RECONNECT_BASE;
    let connected = false;
    let onIdle = null;
    let onConnect = null;
    let onDisconnect = null;

    function wsURL() {
      const proto = location.protocol === "https:" ? "wss:" : "ws:";
      return proto + "//" + location.host + "/mpd";
    }

    function connectCmd() {
      if (cmdWs && cmdWs.readyState <= 1) return;
      cmdWs = new WebSocket(wsURL());
      cmdWs.binaryType = "arraybuffer";
      let buf = "";
      let greeting = false;

      cmdWs.onopen = function () {};
      cmdWs.onmessage = function (ev) {
        const text = typeof ev.data === "string" ? ev.data : new TextDecoder().decode(ev.data);
        buf += text;
        // Consume greeting
        if (!greeting) {
          const idx = buf.indexOf("\n");
          if (idx >= 0) {
            buf = buf.slice(idx + 1);
            greeting = true;
            if (!connected) {
              connected = true;
              reconnectDelay = RECONNECT_BASE;
              if (onConnect) onConnect();
            }
          }
          // Process any remaining buffered data
          if (buf.length > 0) processBuffer();
          return;
        }
        processBuffer();
      };

      function findLineStart(str, token) {
        // Find `token` only at the start of a line (pos 0 or preceded by \n)
        var pos = 0;
        while (pos < str.length) {
          var idx = str.indexOf(token, pos);
          if (idx === -1) return -1;
          if (idx === 0 || str[idx - 1] === "\n") return idx;
          pos = idx + 1;
        }
        return -1;
      }

      function processBuffer() {
        // Look for OK or ACK at start of line to resolve pending command
        while (cmdQueue.length > 0) {
          var okIdx = findLineStart(buf, "OK\n");
          var ackIdx = findLineStart(buf, "ACK ");

          if (okIdx === -1 && ackIdx === -1) break;

          let endIdx, isError;
          if (ackIdx >= 0 && (okIdx === -1 || ackIdx < okIdx)) {
            // Find end of ACK line
            const ackEnd = buf.indexOf("\n", ackIdx);
            if (ackEnd === -1) break;
            endIdx = ackEnd + 1;
            isError = true;
          } else {
            endIdx = okIdx + 3;
            isError = false;
          }

          const chunk = buf.slice(0, endIdx);
          buf = buf.slice(endIdx);
          const entry = cmdQueue.shift();
          if (isError) {
            entry.reject(new Error(chunk.trim()));
          } else {
            entry.resolve(chunk);
          }
        }
      }

      cmdWs.onclose = function () {
        connected = false;
        // Reject all pending commands so callers don't hang
        while (cmdQueue.length > 0) {
          var entry = cmdQueue.shift();
          entry.reject(new Error("disconnected"));
        }
        buf = "";
        if (onDisconnect) onDisconnect();
        scheduleReconnect();
      };
      cmdWs.onerror = function () {};
    }

    function connectIdle() {
      if (idleWs && idleWs.readyState <= 1) return;
      idleWs = new WebSocket(wsURL());
      idleWs.binaryType = "arraybuffer";
      let buf = "";
      let greeting = false;
      let idling = false;

      idleWs.onopen = function () {};
      idleWs.onmessage = function (ev) {
        const text = typeof ev.data === "string" ? ev.data : new TextDecoder().decode(ev.data);
        buf += text;
        if (!greeting) {
          const idx = buf.indexOf("\n");
          if (idx >= 0) {
            buf = buf.slice(idx + 1);
            greeting = true;
            startIdle();
          }
          return;
        }
        processIdleBuffer();
      };

      function processIdleBuffer() {
        // Look for OK\n which terminates an idle response
        const okIdx = buf.indexOf("OK\n");
        if (okIdx === -1) return;
        const chunk = buf.slice(0, okIdx);
        buf = buf.slice(okIdx + 3);
        // Parse changed subsystems
        const changed = [];
        for (const line of chunk.split("\n")) {
          if (line.startsWith("changed: ")) {
            changed.push(line.slice(9).trim());
          }
        }
        if (changed.length > 0 && onIdle) {
          onIdle(changed);
        }
        // Re-enter idle
        startIdle();
      }

      function startIdle() {
        if (idleWs.readyState === 1) {
          idleWs.send("idle player playlist mixer options stored_playlist database rating output\n");
          idling = true;
        }
      }

      idleWs.onclose = function () {
        setTimeout(connectIdle, reconnectDelay);
      };
      idleWs.onerror = function () {};
    }

    function scheduleReconnect() {
      setTimeout(function () {
        reconnectDelay = Math.min(reconnectDelay * 2, RECONNECT_MAX);
        connectCmd();
      }, reconnectDelay);
    }

    function cmd(command) {
      return new Promise(function (resolve, reject) {
        if (!cmdWs || cmdWs.readyState !== 1) {
          reject(new Error("not connected"));
          return;
        }
        cmdQueue.push({ resolve: resolve, reject: reject });
        cmdWs.send(command + "\n");
      });
    }

    function parseKV(text) {
      const result = [];
      for (const line of text.split("\n")) {
        const idx = line.indexOf(": ");
        if (idx > 0) {
          result.push([line.slice(0, idx), line.slice(idx + 2)]);
        }
      }
      return result;
    }

    function parseOne(text) {
      const obj = {};
      for (const [k, v] of parseKV(text)) {
        obj[k] = v;
      }
      return obj;
    }

    // Parse grouped responses (multiple entries delimited by a key)
    function parseList(text, delimKey) {
      const items = [];
      let current = null;
      for (const [k, v] of parseKV(text)) {
        if (k === delimKey) {
          if (current) items.push(current);
          current = {};
        }
        if (!current) current = {};
        current[k] = v;
      }
      if (current) items.push(current);
      return items;
    }

    return {
      connect: function () { connectCmd(); connectIdle(); },
      cmd: cmd,
      parseKV: parseKV,
      parseOne: parseOne,
      parseList: parseList,
      set onIdle(fn) { onIdle = fn; },
      set onConnect(fn) { onConnect = fn; },
      set onDisconnect(fn) { onDisconnect = fn; },
      get connected() { return connected; },
    };
  }

  // -------------------------------------------------------------------------
  // State Store
  // -------------------------------------------------------------------------

  const state = {};
  const listeners = {};

  function set(key, value) {
    state[key] = value;
    if (listeners[key]) {
      for (const fn of listeners[key]) fn(value);
    }
  }

  function get(key) { return state[key]; }

  function subscribe(key, fn) {
    if (!listeners[key]) listeners[key] = [];
    listeners[key].push(fn);
  }

  // -------------------------------------------------------------------------
  // MPD API
  // -------------------------------------------------------------------------

  const mpd = createMPD();

  async function getStatus() {
    const raw = await mpd.cmd("status");
    return mpd.parseOne(raw);
  }

  async function getCurrentSong() {
    const raw = await mpd.cmd("currentsong");
    return mpd.parseOne(raw);
  }

  async function getArtists() {
    const raw = await mpd.cmd("list AlbumArtist");
    return mpd.parseKV(raw).map(function (p) { return p[1]; }).filter(Boolean);
  }

  async function getAllAlbumsLatest() {
    const raw = await mpd.cmd("list Album group Date group AlbumArtist sort latest");
    const items = [];
    let cur = null;
    for (const [k, v] of mpd.parseKV(raw)) {
      if (k === "AlbumArtist") {
        if (cur && cur.Album) items.push(cur);
        cur = { AlbumArtist: v };
      } else if (k === "Date") {
        if (!cur) cur = {};
        cur.Date = v;
      } else if (k === "Album") {
        if (!cur) cur = {};
        cur.Album = v;
      } else if (k === "X-AlbumId") {
        if (cur) cur["X-AlbumId"] = v;
      }
      if (items.length >= 50) break;
    }
    if (cur && cur.Album && items.length < 50) items.push(cur);
    return items;
  }

  async function getAlbums(artist) {
    const escaped = artist.replace(/"/g, '\\"');
    const raw = await mpd.cmd('list Album AlbumArtist "' + escaped + '" group Date group AlbumArtist');
    const items = [];
    let cur = null;
    for (const [k, v] of mpd.parseKV(raw)) {
      // Group tags (AlbumArtist, Date) come BEFORE Album in MPD output,
      // so start a new pending object on the first group tag after an Album.
      if (k === "AlbumArtist") {
        if (cur && cur.Album) items.push(cur);
        cur = { AlbumArtist: v };
      } else if (k === "Date") {
        if (!cur) cur = {};
        cur.Date = v;
      } else if (k === "Album") {
        if (!cur) cur = {};
        cur.Album = v;
      } else if (k === "X-AlbumId") {
        if (cur) cur["X-AlbumId"] = v;
      }
    }
    if (cur && cur.Album) items.push(cur);
    return items;
  }

  async function getTracks(artist, album) {
    const ea = artist.replace(/"/g, '\\"');
    const eb = album.replace(/"/g, '\\"');
    const raw = await mpd.cmd('find AlbumArtist "' + ea + '" Album "' + eb + '"');
    return mpd.parseList(raw, "file");
  }

  async function searchAll(query, ratingOp, ratingVal) {
    var filter = "";
    if (query) {
      var eq = query.replace(/"/g, '\\"');
      filter += ' "(any contains \\"' + eq + '\\")"';
    }
    if (ratingOp && ratingVal) {
      filter += ' "(rating ' + ratingOp + " " + ratingVal + ')"';
    }
    if (!filter) return [];
    var raw = await mpd.cmd("search" + filter);
    return mpd.parseList(raw, "file");
  }

  async function getQueue() {
    const raw = await mpd.cmd("playlistinfo");
    return mpd.parseList(raw, "file");
  }

  async function getPlaylists() {
    const raw = await mpd.cmd("listplaylists");
    return mpd.parseList(raw, "playlist");
  }

  async function getPlaylistTracks(name) {
    const en = name.replace(/"/g, '\\"');
    const raw = await mpd.cmd('listplaylistinfo "' + en + '"');
    return mpd.parseList(raw, "file");
  }


  // Playback controls
  async function play(pos) { await mpd.cmd(pos !== undefined ? "play " + pos : "play"); }
  async function pause() { await mpd.cmd("pause"); }
  async function stop() { await mpd.cmd("stop"); }
  async function next() { await mpd.cmd("next"); }
  async function prev() { await mpd.cmd("previous"); }
  async function seekCur(time) { await mpd.cmd("seekcur " + time); }

  async function setVolume(vol) { await mpd.cmd("setvol " + vol); }

  // Queue management
  async function addToQueue(uri) {
    const eu = uri.replace(/"/g, '\\"');
    await mpd.cmd('add "' + eu + '"');
  }

  async function addIdToQueue(uri, pos) {
    const eu = uri.replace(/"/g, '\\"');
    if (pos !== undefined) {
      await mpd.cmd('addid "' + eu + '" ' + pos);
    } else {
      await mpd.cmd('addid "' + eu + '"');
    }
  }

  async function findAdd(artist, album) {
    const ea = artist.replace(/"/g, '\\"');
    const eb = album.replace(/"/g, '\\"');
    await mpd.cmd('findadd AlbumArtist "' + ea + '" Album "' + eb + '"');
  }

  async function clearQueue() { await mpd.cmd("clear"); }
  async function removeFromQueue(pos) { await mpd.cmd("delete " + pos); }
  async function moveInQueue(from, to) { await mpd.cmd("move " + from + " " + to); }

  // Ratings
  async function rateTrack(songId, rating) { await mpd.cmd("rate " + songId + " " + rating); }
  async function getTrackRating(songId) {
    const raw = await mpd.cmd("getrating " + songId);
    const obj = mpd.parseOne(raw);
    return parseInt(obj.rating || "0", 10);
  }
  async function rateAlbum(artist, album, date, rating) {
    const ea = artist.replace(/"/g, '\\"');
    const eb = album.replace(/"/g, '\\"');
    const ed = (date || "").replace(/"/g, '\\"');
    await mpd.cmd('albumrate "' + ea + '" "' + eb + '" "' + ed + '" ' + rating);
  }
  async function getAlbumRating(artist, album, date) {
    const ea = artist.replace(/"/g, '\\"');
    const eb = album.replace(/"/g, '\\"');
    const ed = (date || "").replace(/"/g, '\\"');
    const raw = await mpd.cmd('getalbumrating "' + ea + '" "' + eb + '" "' + ed + '"');
    return mpd.parseOne(raw);
  }

  // Stored playlists
  async function loadPlaylist(name) {
    const en = name.replace(/"/g, '\\"');
    await mpd.cmd('load "' + en + '"');
  }
  async function addToPlaylist(name, uri) {
    const en = name.replace(/"/g, '\\"');
    const eu = uri.replace(/"/g, '\\"');
    await mpd.cmd('playlistadd "' + en + '" "' + eu + '"');
  }


  // -------------------------------------------------------------------------
  // Audio Controller
  // -------------------------------------------------------------------------

  const audio = document.getElementById("player");
  let currentStreamId = null;
  let audioUnlocked = false;

  // Browsers block audio.play() unless it originates from a user gesture.
  // We create a silent AudioContext on the first click to permanently unlock audio.
  let audioCtx = null;
  let gainNode = null;
  var rgMode = "off";
  var rgTrackGain = 0;
  var rgAlbumGain = 0;
  document.addEventListener("click", function () {
    if (audioUnlocked) return;
    try {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      gainNode = audioCtx.createGain();
      var source = audioCtx.createMediaElementSource(audio);
      source.connect(gainNode);
      gainNode.connect(audioCtx.destination);
      audioUnlocked = true;
    } catch (e) {
      audioUnlocked = true;
    }
  }, { once: true });

  var advancing = false;

  function loadTrack(songId) {
    if (!songId) return;
    advancing = false;
    currentStreamId = songId;
    lastReportedTime = 0;
    audio.src = "/api/v1/stream/" + songId;
    audio.load();
    audio.play().catch(function () {});
  }

  function stopAudio() {
    audio.pause();
    audio.removeAttribute("src");
    audio.load();
    currentStreamId = null;
  }

  function advanceToNext() {
    if (advancing) return;
    advancing = true;
    currentStreamId = null;
    mpd.cmd("trackended").then(function () {
      refreshNowPlaying();
    });
  }

  audio.addEventListener("ended", function () {
    advanceToNext();
  });

  // Report time position to server periodically so status stays in sync
  var lastReportedTime = 0;
  audio.addEventListener("timeupdate", function () {
    if (!audio.duration) return;

    // Detect end of track (within 0.5s of end and not already advancing)
    if (audio.duration - audio.currentTime < 0.5 && audio.currentTime > 1 && !advancing) {
      advanceToNext();
      return;
    }

    if (advancing) return;

    var seekBar = document.getElementById("seek-bar");
    if (seekBar && !seekBar.dataset.dragging) {
      seekBar.value = (audio.currentTime / audio.duration) * 100;
    }
    var cur = document.getElementById("np-time-current");
    if (cur) cur.textContent = formatTime(audio.currentTime);

    // Report to server every ~5 seconds
    if (Math.abs(audio.currentTime - lastReportedTime) > 5) {
      lastReportedTime = audio.currentTime;
      mpd.cmd("seekcur " + audio.currentTime).catch(function () {});
    }
  });

  audio.addEventListener("loadedmetadata", function () {
    var tot = document.getElementById("np-time-total");
    if (tot) tot.textContent = formatTime(audio.duration);
  });

  function applyReplayGain() {
    if (!gainNode) return;
    var db = 0;
    if (rgMode === "track") db = rgTrackGain;
    else if (rgMode === "album") db = rgAlbumGain;
    gainNode.gain.value = db !== 0 ? Math.pow(10, db / 20) : 1.0;
  }

  // -------------------------------------------------------------------------
  // Auth
  // -------------------------------------------------------------------------

  async function tryAuth(secret) {
    const resp = await fetch("/web/auth", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: "secret=" + encodeURIComponent(secret),
    });
    return resp.ok;
  }

  // -------------------------------------------------------------------------
  // UI Rendering
  // -------------------------------------------------------------------------

  const $content = document.getElementById("content");
  const $loginView = document.getElementById("login-view");
  const $app = document.getElementById("app");

  // Navigation state
  let currentView = "library";
  let navArtist = null;
  let navAlbum = null;
  let navAlbumDate = null;
  let libSortLatest = false;

  // --- Login ---

  document.getElementById("login-form").addEventListener("submit", async function (e) {
    e.preventDefault();
    const input = document.getElementById("login-secret");
    const errEl = document.getElementById("login-error");
    errEl.classList.add("hidden");
    const ok = await tryAuth(input.value);
    if (ok) {
      $loginView.style.display = "none";
      $loginView.classList.add("hidden");
      $app.style.display = "";
      mpd.connect();
    } else {
      errEl.textContent = "Invalid secret";
      errEl.classList.remove("hidden");
    }
  });

  // --- Sidebar ---

  document.querySelectorAll(".nav-item").forEach(function (el) {
    el.addEventListener("click", function () {
      const view = el.dataset.view;
      navigateTo(view);
    });
  });

  function navigateTo(view, opts) {
    currentView = view;
    document.querySelectorAll(".nav-item").forEach(function (el) {
      el.classList.toggle("active", el.dataset.view === view);
    });
    navArtist = null;
    navAlbum = null;
    navAlbumDate = null;
    libSortLatest = false;
    if (opts) {
      navArtist = opts.artist || null;
      navAlbum = opts.album || null;
      navAlbumDate = opts.date || null;
    }

    switch (view) {
      case "library": renderLibrary(); break;
      case "search": renderSearch(); break;
      case "queue": renderQueue(); break;
      case "playlists": renderPlaylists(); break;
    }
  }

  // --- Library ---

  async function renderLibrary() {
    if (navAlbum && navArtist) {
      await renderTracks(navArtist, navAlbum, navAlbumDate);
    } else if (navArtist) {
      await renderAlbums(navArtist);
    } else if (libSortLatest) {
      await renderAllAlbumsLatest();
    } else {
      await renderArtists();
    }
  }

  async function renderArtists() {
    $content.innerHTML = '<div class="view-header"><h2>Artists</h2></div><div class="loading">Loading...</div>';
    try {
      const artists = await getArtists();
      let html = '<div class="view-header"><h2>Artists</h2><span class="badge">' + artists.length + "</span>";
      html += '<button class="btn-ghost" id="lib-sort-toggle">Latest</button></div>';
      html += '<div class="artist-list">';
      for (const a of artists) {
        html += '<div class="artist-item" data-artist="' + escAttr(a) + '">' + esc(a) + "</div>";
      }
      html += "</div>";
      $content.innerHTML = html;
      document.getElementById("lib-sort-toggle").addEventListener("click", function () {
        libSortLatest = true;
        renderAllAlbumsLatest();
      });
      $content.querySelectorAll(".artist-item").forEach(function (el) {
        el.addEventListener("click", function () {
          navArtist = el.dataset.artist;
          renderAlbums(navArtist);
        });
      });
    } catch (e) {
      $content.innerHTML = '<div class="error">Failed to load artists</div>';
    }
  }

  async function renderAllAlbumsLatest() {
    $content.innerHTML = '<div class="view-header"><h2>Albums</h2></div><div class="loading">Loading...</div>';
    try {
      const albums = await getAllAlbumsLatest();
      let html = '<div class="view-header">';
      html += '<button class="btn-ghost" id="latest-sort-toggle">Artists</button>';
      html += "<h2>Latest Albums</h2>";
      html += '<span class="badge">' + albums.length + "</span></div>";
      html += '<div class="album-grid">';
      for (const al of albums) {
        const artist = al.AlbumArtist || "";
        html += '<div class="album-card" data-artist="' + escAttr(artist) + '" data-album="' + escAttr(al.Album) + '" data-date="' + escAttr(al.Date || "") + '">';
        const albumId = al["X-AlbumId"] || "";
        const coverSrc = albumId ? "/api/v1/cover/" + albumId : "";
        html += '<div class="album-art-wrap"><img class="album-art" src="' + escAttr(coverSrc) + '" alt="" loading="lazy" onerror="this.style.display=\'none\'"></div>';
        html += '<div class="album-name">' + esc(al.Album) + "</div>";
        html += '<div class="album-date">' + esc(artist);
        if (al.Date) html += ' \u00B7 ' + esc(al.Date);
        html += "</div>";
        html += "</div>";
      }
      html += "</div>";
      $content.innerHTML = html;

      document.getElementById("latest-sort-toggle").addEventListener("click", function () {
        libSortLatest = false;
        renderArtists();
      });
      $content.querySelectorAll(".album-card").forEach(function (el) {
        el.addEventListener("click", function () {
          navArtist = el.dataset.artist;
          navAlbum = el.dataset.album;
          navAlbumDate = el.dataset.date;
          renderTracks(navArtist, navAlbum, navAlbumDate);
        });
        el.addEventListener("contextmenu", function (e) {
          e.preventDefault();
          const artist = el.dataset.artist;
          const album = el.dataset.album;
          showContextMenu(e.clientX, e.clientY, [
            { label: "Play", action: function () { replaceQueueAlbum(artist, album); } },
            { label: "Add to queue", action: function () { addAlbumToQueue(artist, album); } },
            { label: "Add next", action: function () { insertAlbumNext(artist, album); } },
          ]);
        });
      });
    } catch (e) {
      $content.innerHTML = '<div class="error">Failed to load albums</div>';
    }
  }

  async function renderAlbums(artist) {
    navArtist = artist;
    $content.innerHTML = '<div class="view-header"><h2>Albums</h2></div><div class="loading">Loading...</div>';
    try {
      const albums = await getAlbums(artist);
      let html = '<div class="view-header">';
      html += '<button class="btn-back" id="albums-back">\u25C0</button>';
      html += "<h2>" + esc(artist) + "</h2>";
      html += '<span class="badge">' + albums.length + "</span></div>";
      html += '<div class="album-grid">';
      for (const al of albums) {
        const albumId = al._albumId || "";
        html += '<div class="album-card" data-album="' + escAttr(al.Album) + '" data-date="' + escAttr(al.Date || "") + '">';
        html += '<div class="album-art-wrap"><img class="album-art" data-artist="' + escAttr(artist) + '" data-album="' + escAttr(al.Album) + '" src="" alt="" loading="lazy"></div>';
        html += '<div class="album-name">' + esc(al.Album) + "</div>";
        if (al.Date) html += '<div class="album-date">' + esc(al.Date) + "</div>";
        html += "</div>";
      }
      html += "</div>";
      $content.innerHTML = html;

      // Load cover art for albums — need album IDs from track lookup
      loadAlbumCovers(artist, albums);

      document.getElementById("albums-back").addEventListener("click", function () {
        navArtist = null;
        renderArtists();
      });
      $content.querySelectorAll(".album-card").forEach(function (el) {
        el.addEventListener("click", function () {
          navAlbum = el.dataset.album;
          navAlbumDate = el.dataset.date;
          renderTracks(navArtist, navAlbum, navAlbumDate);
        });
        el.addEventListener("contextmenu", function (e) {
          e.preventDefault();
          showContextMenu(e.clientX, e.clientY, [
            { label: "Play", action: function () { replaceQueueAlbum(artist, el.dataset.album); } },
            { label: "Add to queue", action: function () { addAlbumToQueue(artist, el.dataset.album); } },
            { label: "Add next", action: function () { insertAlbumNext(artist, el.dataset.album); } },
          ]);
        });
      });
    } catch (e) {
      $content.innerHTML = '<div class="error">Failed to load albums</div>';
    }
  }

  async function loadAlbumCovers(artist, albums) {
    // For each album, find the first track to get album_id for cover art
    for (const al of albums) {
      try {
        const tracks = await getTracks(artist, al.Album);
        if (tracks.length > 0 && tracks[0]["X-AlbumId"]) {
          const imgs = $content.querySelectorAll('img[data-album="' + CSS.escape(al.Album) + '"][data-artist="' + CSS.escape(artist) + '"]');
          imgs.forEach(function (img) {
            img.src = "/api/v1/cover/" + tracks[0]["X-AlbumId"];
            img.onerror = function () { img.style.display = "none"; };
          });
        }
      } catch (e) { /* ignore */ }
    }
  }

  async function renderTracks(artist, album, date) {
    navArtist = artist;
    navAlbum = album;
    navAlbumDate = date;
    $content.innerHTML = '<div class="view-header"><h2>Tracks</h2></div><div class="loading">Loading...</div>';
    try {
      const tracks = await getTracks(artist, album);
      let albumRating = null;
      try {
        albumRating = await getAlbumRating(artist, album, date || "");
      } catch (e) { /* ignore */ }

      // Get album ID for cover art from first track
      const albumId = tracks.length > 0 ? (tracks[0]["X-AlbumId"] || "") : "";

      let html = '<div class="album-detail-header">';
      if (albumId) {
        html += '<img class="album-detail-art" src="/api/v1/cover/' + escAttr(albumId) + '" alt="" onerror="this.style.display=\'none\'">';
      }
      html += '<div class="album-detail-info">';
      html += '<button class="btn-back" id="tracks-back">\u25C0</button>';
      html += "<h2>" + esc(album) + "</h2>";
      html += '<div class="view-sub">' + esc(artist);
      if (date) html += " \u00B7 " + esc(date);
      html += "</div>";

      // Album rating
      html += '<div class="album-rating-row">';
      html += '<span class="label">Album rating:</span>';
      html += renderRatingStars("album-rating", albumRating ? parseInt(albumRating.rating || "0", 10) : 0);
      if (albumRating && albumRating.computed) {
        html += '<span class="computed-rating">avg ' + parseFloat(albumRating.computed).toFixed(1) + "</span>";
      }
      html += "</div>";

      // Album actions
      html += '<div class="album-actions">';
      html += '<button class="btn-sm" id="album-play">Play</button>';
      html += '<button class="btn-sm" id="album-add">Add</button>';
      html += '<button class="btn-sm" id="album-insert">Insert</button>';
      html += "</div>";
      html += "</div></div>";

      html += '<div class="track-list">';
      for (let i = 0; i < tracks.length; i++) {
        const t = tracks[i];
        const trackNum = t.Track || (i + 1);
        const rating = parseInt(t["X-Rating"] || "0", 10);
        html += '<div class="track-item" data-uri="' + escAttr(t.file) + '" data-pos="' + i + '" data-songid="' + escAttr(t["X-SongId"] || "") + '">';
        html += '<span class="track-num">' + trackNum + "</span>";
        html += '<span class="track-title">' + esc(t.Title || t.file) + "</span>";
        html += '<span class="track-duration">' + esc(formatDuration(t.duration || t.Time)) + "</span>";
        html += '<span class="track-rating" data-songid="' + escAttr(t["X-SongId"] || "") + '">' + renderRatingDots(rating) + "</span>";
        html += "</div>";
      }
      html += "</div>";
      $content.innerHTML = html;

      document.getElementById("tracks-back").addEventListener("click", function () {
        navAlbum = null;
        if (libSortLatest) {
          navArtist = null;
          renderAllAlbumsLatest();
        } else {
          renderAlbums(navArtist);
        }
      });
      document.getElementById("album-play").addEventListener("click", function () { replaceQueueAlbum(artist, album); });
      document.getElementById("album-add").addEventListener("click", function () { addAlbumToQueue(artist, album); });
      document.getElementById("album-insert").addEventListener("click", function () { insertAlbumNext(artist, album); });

      // Track click = play, right-click = menu
      $content.querySelectorAll(".track-item").forEach(function (el) {
        el.addEventListener("click", function () {
          playTrack(el.dataset.uri, tracks, parseInt(el.dataset.pos, 10));
        });
        el.addEventListener("contextmenu", function (e) {
          e.preventDefault();
          showContextMenu(e.clientX, e.clientY, [
            { label: "Play", action: function () { playTrack(el.dataset.uri, tracks, parseInt(el.dataset.pos, 10)); } },
            { label: "Add to queue", action: function () { addToQueue(el.dataset.uri); } },
            { label: "Add next", action: function () { insertTrackNext(el.dataset.uri); } },
          ]);
        });
      });

      // Album rating stars interaction
      setupAlbumRatingStars(artist, album, date);

      // Track rating dots interaction
      setupTrackRatingDots();

    } catch (e) {
      $content.innerHTML = '<div class="error">Failed to load tracks</div>';
    }
  }

  // --- Search ---

  let searchTimeout = null;
  let lastSearchQuery = "";
  let searchRatingOp = "";
  let searchRatingVal = "";

  function renderSearch() {
    let html = '<div class="view-header"><h2>Search</h2></div>';
    html += '<div class="search-bar">';
    html += '<input type="text" id="search-input" class="search-input" placeholder="Search..." value="' + escAttr(lastSearchQuery) + '">';
    html += '<div class="rating-filter">';
    html += '<select id="search-rating-op"><option value="">Rating</option><option value=">=">≥</option><option value="<=">≤</option><option value=">">></option><option value="<"><</option><option value="==">=</option></select>';
    html += '<select id="search-rating-val"><option value="">-</option>';
    for (let i = 1; i <= 10; i++) html += '<option value="' + i + '">' + i + "</option>";
    html += "</select></div></div>";
    html += '<div id="search-results"></div>';
    $content.innerHTML = html;

    const input = document.getElementById("search-input");
    const opSel = document.getElementById("search-rating-op");
    const valSel = document.getElementById("search-rating-val");

    opSel.value = searchRatingOp;
    valSel.value = searchRatingVal;

    function doSearch() {
      lastSearchQuery = input.value;
      searchRatingOp = opSel.value;
      searchRatingVal = valSel.value;
      performSearch(input.value, opSel.value, valSel.value);
    }

    input.addEventListener("input", function () {
      clearTimeout(searchTimeout);
      searchTimeout = setTimeout(doSearch, 300);
    });
    opSel.addEventListener("change", doSearch);
    valSel.addEventListener("change", doSearch);

    input.focus();
    if (lastSearchQuery || searchRatingOp) doSearch();
  }

  async function performSearch(query, ratingOp, ratingVal) {
    const results = document.getElementById("search-results");
    if (!results) return;
    if (!query && !ratingOp) {
      results.innerHTML = "";
      return;
    }
    results.innerHTML = '<div class="loading">Searching...</div>';
    try {
      var allTracks = await searchAll(query, ratingOp, ratingVal);

      if (allTracks.length === 0) {
        results.innerHTML = '<div class="empty">No results</div>';
        return;
      }

      var queryLower = (query || "").toLowerCase();

      // Split: albums where albumartist or album name matches.
      // Tracks: show all results, sorted by artist match first.
      var albumMap = new Map();
      for (var i = 0; i < allTracks.length; i++) {
        var t = allTracks[i];
        var artist = t.AlbumArtist || t.Artist || "";
        var album = t.Album || "";
        var date = t.Date || "";
        var albumMatch = queryLower && (artist.toLowerCase().indexOf(queryLower) >= 0 || album.toLowerCase().indexOf(queryLower) >= 0);

        if (albumMatch || !queryLower) {
          var key = artist + "\0" + album + "\0" + date;
          if (!albumMap.has(key)) {
            albumMap.set(key, { artist: artist, album: album, date: date, albumId: t["X-AlbumId"] || "" });
          }
        }
      }

      // Sort tracks: artist name matches first, then the rest
      var titleTracks = allTracks.slice();
      if (queryLower) {
        titleTracks.sort(function (a, b) {
          var aArtist = ((a.Artist || a.AlbumArtist || "").toLowerCase().indexOf(queryLower) >= 0) ? 0 : 1;
          var bArtist = ((b.Artist || b.AlbumArtist || "").toLowerCase().indexOf(queryLower) >= 0) ? 0 : 1;
          return aArtist - bArtist;
        });
      }

      var html = "";

      // Albums section
      if (albumMap.size > 0) {
        html += '<div class="search-section"><div class="search-section-title">Albums</div>';
        html += '<div class="search-album-list">';
        for (var entry of albumMap) {
          var info = entry[1];
          html += '<div class="search-album-item" data-artist="' + escAttr(info.artist) + '" data-album="' + escAttr(info.album) + '" data-date="' + escAttr(info.date) + '">';
          if (info.albumId) {
            html += '<img class="search-album-art" src="/api/v1/cover/' + escAttr(info.albumId) + '" alt="" loading="lazy" onerror="this.style.display=\'none\'">';
          } else {
            html += '<div class="search-album-art-placeholder"></div>';
          }
          html += '<div class="search-album-info">';
          html += '<div class="search-album-name">' + esc(info.album) + '</div>';
          html += '<div class="search-album-artist">' + esc(info.artist);
          if (info.date) html += ' \u00B7 ' + esc(info.date);
          html += '</div></div></div>';
        }
        html += '</div></div>';
      }

      // Tracks section (title matches only)
      if (titleTracks.length > 0) {
        html += '<div class="search-section"><div class="search-section-title">Tracks</div>';
        html += '<div class="track-list">';
        for (var j = 0; j < titleTracks.length; j++) {
          var tr = titleTracks[j];
          var rating = parseInt(tr["X-Rating"] || "0", 10);
          html += '<div class="track-item" data-uri="' + escAttr(tr.file) + '">';
          html += '<span class="track-title">' + esc(tr.Title || tr.file) + '</span>';
          html += '<span class="track-artist">' + esc(tr.AlbumArtist || tr.Artist || "") + '</span>';
          html += '<span class="track-album">' + esc(tr.Album || "") + '</span>';
          html += '<span class="track-duration">' + esc(formatDuration(tr.duration || tr.Time)) + '</span>';
          html += '<span class="track-rating">' + renderRatingDots(rating) + '</span>';
          html += '</div>';
        }
        html += '</div></div>';
      }

      results.innerHTML = html;

      // Album click → navigate to album
      results.querySelectorAll(".search-album-item").forEach(function (el) {
        el.addEventListener("click", function () {
          navigateTo("library", {
            artist: el.dataset.artist,
            album: el.dataset.album,
            date: el.dataset.date,
          });
        });
        el.addEventListener("contextmenu", function (e) {
          e.preventDefault();
          const artist = el.dataset.artist;
          const album = el.dataset.album;
          showContextMenu(e.clientX, e.clientY, [
            { label: "Play", action: function () { replaceQueueAlbum(artist, album); } },
            { label: "Add to queue", action: function () { addAlbumToQueue(artist, album); } },
            { label: "Add next", action: function () { insertAlbumNext(artist, album); } },
          ]);
        });
      });

      // Track click → add to queue
      results.querySelectorAll(".track-item").forEach(function (el) {
        el.addEventListener("click", function () {
          addToQueue(el.dataset.uri);
          el.classList.add("added");
          setTimeout(function () { el.classList.remove("added"); }, 500);
        });
        el.addEventListener("contextmenu", function (e) {
          e.preventDefault();
          showContextMenu(e.clientX, e.clientY, [
            { label: "Add to queue", action: function () { addToQueue(el.dataset.uri); } },
            { label: "Add next", action: function () { insertTrackNext(el.dataset.uri); } },
            { label: "Play now", action: function () { playTrackByURI(el.dataset.uri); } },
          ]);
        });
      });
    } catch (e) {
      results.innerHTML = '<div class="error">Search failed</div>';
    }
  }

  // --- Queue ---

  async function renderQueue() {
    $content.innerHTML = '<div class="view-header"><h2>Queue</h2></div><div class="loading">Loading...</div>';
    try {
      const queue = await getQueue();
      const status = await getStatus();
      const currentPos = parseInt(status.song || "-1", 10);

      let html = '<div class="view-header"><h2>Queue</h2><span class="badge">' + queue.length + "</span>";
      html += '<button class="btn-ghost" id="queue-clear-main">Clear</button></div>';
      if (queue.length === 0) {
        html += '<div class="empty">Queue is empty</div>';
        $content.innerHTML = html;
        document.getElementById("queue-clear-main").addEventListener("click", function () { clearQueue().then(renderQueue); });
        return;
      }
      html += '<table class="queue-table"><thead><tr>';
      html += '<th class="qt-pos">#</th>';
      html += '<th class="qt-title">Title</th>';
      html += '<th class="qt-artist">Artist</th>';
      html += '<th class="qt-album">Album</th>';
      html += '<th class="qt-year">Year</th>';
      html += '<th class="qt-dur">Duration</th>';
      html += '<th class="qt-act"></th>';
      html += '</tr></thead><tbody>';
      for (let i = 0; i < queue.length; i++) {
        const t = queue[i];
        const active = i === currentPos ? " active" : "";
        html += '<tr class="queue-row' + active + '" data-pos="' + i + '" data-id="' + escAttr(t.Id) + '">';
        html += '<td class="qt-pos">' + (i === currentPos ? '<span class="now-playing-icon">\u266A</span>' : (i + 1)) + "</td>";
        html += '<td class="qt-title">' + esc(t.Title || t.file) + "</td>";
        html += '<td class="qt-artist">' + esc(t.Artist || t.AlbumArtist || "") + "</td>";
        html += '<td class="qt-album">' + esc(t.Album || "") + "</td>";
        html += '<td class="qt-year">' + esc(t.Date || "") + "</td>";
        html += '<td class="qt-dur">' + esc(formatDuration(t.duration || t.Time)) + "</td>";
        html += '<td class="qt-act"><button class="btn-remove" data-pos="' + i + '">\u2715</button></td>';
        html += "</tr>";
      }
      html += "</tbody></table>";
      $content.innerHTML = html;

      document.getElementById("queue-clear-main").addEventListener("click", function () { clearQueue().then(renderQueue); });

      // Click to play
      $content.querySelectorAll(".queue-row").forEach(function (el) {
        el.addEventListener("click", function (e) {
          if (e.target.classList.contains("btn-remove")) return;
          currentStreamId = null;
          play(parseInt(el.dataset.pos, 10));
        });
      });

      // Remove buttons
      $content.querySelectorAll(".btn-remove").forEach(function (el) {
        el.addEventListener("click", function (e) {
          e.stopPropagation();
          removeFromQueue(parseInt(el.dataset.pos, 10)).then(function () {
            if (currentView === "queue") renderQueue();
          });
        });
      });
    } catch (e) {
      $content.innerHTML = '<div class="error">Failed to load queue</div>';
    }
  }

  // --- Queue Panel (sidebar) ---

  var queuePanelGen = 0;

  async function refreshQueuePanel() {
    const panel = document.getElementById("queue-list");
    if (!panel) return;
    var gen = ++queuePanelGen;
    try {
      const queue = await getQueue();
      if (gen !== queuePanelGen) return; // stale
      const status = await getStatus();
      if (gen !== queuePanelGen) return; // stale
      const currentPos = parseInt(status.song || "-1", 10);

      if (queue.length === 0) {
        panel.innerHTML = '<div class="empty">Empty</div>';
        return;
      }
      let html = "";
      for (let i = 0; i < queue.length; i++) {
        const t = queue[i];
        const active = i === currentPos ? " active" : "";
        html += '<div class="queue-item' + active + '" data-pos="' + i + '">';
        html += '<div class="queue-item-info"><div class="queue-item-title">' + esc(t.Title || t.file) + "</div>";
        html += '<div class="queue-item-artist">' + esc(t.Artist || t.AlbumArtist || "") + "</div></div>";
        html += "</div>";
      }
      panel.innerHTML = html;
      panel.querySelectorAll(".queue-item").forEach(function (el) {
        el.addEventListener("click", function () {
          play(parseInt(el.dataset.pos, 10));
        });
      });
    } catch (e) { /* ignore */ }
  }

  document.getElementById("queue-clear").addEventListener("click", function () {
    clearQueue().then(refreshQueuePanel);
  });

  // --- Playlists ---

  let selectedPlaylist = null;

  async function renderPlaylists() {
    if (selectedPlaylist) {
      await renderPlaylistTracks(selectedPlaylist);
      return;
    }
    $content.innerHTML = '<div class="view-header"><h2>Playlists</h2></div><div class="loading">Loading...</div>';
    try {
      const pls = await getPlaylists();
      let html = '<div class="view-header"><h2>Playlists</h2><span class="badge">' + pls.length + "</span></div>";
      if (pls.length === 0) {
        html += '<div class="empty">No playlists</div>';
        $content.innerHTML = html;
        return;
      }
      html += '<div class="playlist-list">';
      for (const p of pls) {
        html += '<div class="playlist-item" data-name="' + escAttr(p.playlist) + '">';
        html += '<span class="playlist-name">' + esc(p.playlist) + "</span>";
        html += "</div>";
      }
      html += "</div>";
      $content.innerHTML = html;
      $content.querySelectorAll(".playlist-item").forEach(function (el) {
        el.addEventListener("click", function () {
          selectedPlaylist = el.dataset.name;
          renderPlaylistTracks(selectedPlaylist);
        });
        el.addEventListener("contextmenu", function (e) {
          e.preventDefault();
          showContextMenu(e.clientX, e.clientY, [
            { label: "Load", action: function () { loadPlaylist(el.dataset.name).then(refreshAll); } },
          ]);
        });
      });
    } catch (e) {
      $content.innerHTML = '<div class="error">Failed to load playlists</div>';
    }
  }

  async function renderPlaylistTracks(name) {
    $content.innerHTML = '<div class="view-header"><h2>Playlist</h2></div><div class="loading">Loading...</div>';
    try {
      const tracks = await getPlaylistTracks(name);
      let html = '<div class="view-header">';
      html += '<button class="btn-back" id="pl-back">\u25C0</button>';
      html += "<h2>" + esc(name) + "</h2>";
      html += '<span class="badge">' + tracks.length + "</span>";
      html += '<button class="btn-sm" id="pl-load">Load</button></div>';
      html += '<div class="track-list">';
      for (let i = 0; i < tracks.length; i++) {
        const t = tracks[i];
        html += '<div class="track-item" data-uri="' + escAttr(t.file) + '">';
        html += '<span class="track-num">' + (i + 1) + "</span>";
        html += '<span class="track-title">' + esc(t.Title || t.file) + "</span>";
        html += '<span class="track-artist">' + esc(t.Artist || t.AlbumArtist || "") + "</span>";
        html += '<span class="track-duration">' + esc(formatDuration(t.duration || t.Time)) + "</span>";
        html += "</div>";
      }
      html += "</div>";
      $content.innerHTML = html;
      document.getElementById("pl-back").addEventListener("click", function () {
        selectedPlaylist = null;
        renderPlaylists();
      });
      document.getElementById("pl-load").addEventListener("click", function () {
        loadPlaylist(name).then(refreshAll);
      });
      $content.querySelectorAll(".track-item").forEach(function (el) {
        el.addEventListener("click", function () {
          addToQueue(el.dataset.uri);
        });
      });
    } catch (e) {
      $content.innerHTML = '<div class="error">Failed to load playlist</div>';
    }
  }

  // --- Now Playing Bar ---

  var nowPlayingGen = 0;

  async function refreshNowPlaying() {
    var gen = ++nowPlayingGen;
    try {
      const status = await getStatus();
      if (gen !== nowPlayingGen) return;
      const song = await getCurrentSong();
      if (gen !== nowPlayingGen) return;
      set("status", status);
      set("currentSong", song);

      // Update ReplayGain values from current song
      if (song) {
        rgTrackGain = parseFloat(song["X-ReplayGainTrack"] || "0");
        rgAlbumGain = parseFloat(song["X-ReplayGainAlbum"] || "0");
        applyReplayGain();
      }

      const title = document.getElementById("np-title");
      const artist = document.getElementById("np-artist");
      const art = document.getElementById("np-art");
      const playBtn = document.getElementById("play-btn");
      const seekBar = document.getElementById("seek-bar");
      const timeTotal = document.getElementById("np-time-total");
      const ratingEl = document.getElementById("np-rating");

      if (song && song.Title) {
        title.textContent = song.Title;
        artist.textContent = song.Artist || song.AlbumArtist || "";
        if (song["X-AlbumId"]) {
          art.src = "/api/v1/cover/" + song["X-AlbumId"];
          art.style.display = "";
        } else {
          art.style.display = "none";
        }
      } else {
        title.textContent = "Not playing";
        artist.textContent = "";
        art.style.display = "none";
      }

      const isPlaying = status.state === "play";
      playBtn.innerHTML = isPlaying ? "&#9208;" : "&#9654;";

      // Update volume slider
      const volSlider = document.getElementById("volume-slider");
      if (status.volume && volSlider && !volSlider.dataset.dragging) {
        volSlider.value = status.volume;
      }

      // Duration from status
      const dur = parseFloat(status.duration || "0");
      if (dur > 0 && timeTotal) {
        timeTotal.textContent = formatTime(dur);
      }

      // Sync browser audio with server state (only when web is active device)
      if (!webIsActive) {
        if (!audio.paused) stopAudio();
      } else if (song && song["X-SongId"]) {
        var songChanged = currentStreamId !== song["X-SongId"];
        if (songChanged) {
          if (isPlaying) {
            loadTrack(song["X-SongId"]);
          } else {
            advancing = false;
            currentStreamId = song["X-SongId"];
            audio.src = "/api/v1/stream/" + song["X-SongId"];
            audio.load();
            audio.pause();
          }
        } else if (!advancing) {
          if (isPlaying && audio.paused && !audio.ended) {
            audio.play().catch(function () {});
          } else if (!isPlaying && !audio.paused) {
            audio.pause();
          }
        }
      } else if (!song || !song.Title) {
        stopAudio();
      }

      // Now playing rating
      if (ratingEl && song && song["X-SongId"]) {
        const rating = parseInt(song["X-Rating"] || "0", 10);
        ratingEl.innerHTML = renderClickableRatingDots(rating, "np");
        setupNowPlayingRating(song["X-SongId"]);
      } else if (ratingEl) {
        ratingEl.innerHTML = "";
      }

    } catch (e) { /* ignore during reconnect */ }
  }

  // --- Controls ---

  document.getElementById("play-btn").addEventListener("click", function () {
    const status = get("status");
    if (!status) return;
    if (status.state === "play") {
      pause();
    } else {
      play();
    }
  });

  document.getElementById("prev-btn").addEventListener("click", function () { prev(); });
  document.getElementById("next-btn").addEventListener("click", function () { next(); });

  // Seek bar
  const seekBar = document.getElementById("seek-bar");
  seekBar.addEventListener("mousedown", function () { seekBar.dataset.dragging = "1"; });
  seekBar.addEventListener("mouseup", function () { delete seekBar.dataset.dragging; });
  seekBar.addEventListener("change", function () {
    delete seekBar.dataset.dragging;
    if (audio.duration) {
      const newTime = (seekBar.value / 100) * audio.duration;
      audio.currentTime = newTime;
      seekCur(newTime);
    }
  });

  // Volume
  const volSlider = document.getElementById("volume-slider");
  volSlider.addEventListener("mousedown", function () { volSlider.dataset.dragging = "1"; });
  volSlider.addEventListener("mouseup", function () { delete volSlider.dataset.dragging; });
  volSlider.addEventListener("input", function () {
    audio.volume = volSlider.value / 100;
  });
  volSlider.addEventListener("change", function () {
    delete volSlider.dataset.dragging;
    setVolume(volSlider.value);
  });

  // -------------------------------------------------------------------------
  // Queue operations
  // -------------------------------------------------------------------------

  function enableWebCmd() {
    if (webDeviceId) {
      webIsActive = true;
      return "enableoutput " + webDeviceId + "\n";
    }
    return "";
  }

  async function playTrack(uri, allTracks, fromIdx) {
    var cmds = "command_list_begin\n" + enableWebCmd() + "clear\n";
    for (const t of allTracks) {
      var eu = t.file.replace(/"/g, '\\"');
      cmds += 'add "' + eu + '"\n';
    }
    cmds += "play " + fromIdx + "\ncommand_list_end";
    currentStreamId = null;
    await mpd.cmd(cmds);
    refreshAll();
  }

  async function playTrackByURI(uri) {
    const eu = uri.replace(/"/g, '\\"');
    currentStreamId = null;
    await mpd.cmd("command_list_begin\n" + enableWebCmd() + "clear\nadd \"" + eu + "\"\nplay 0\ncommand_list_end");
    refreshAll();
  }

  async function replaceQueueAlbum(artist, album) {
    const ea = artist.replace(/"/g, '\\"');
    const eb = album.replace(/"/g, '\\"');
    currentStreamId = null;
    await mpd.cmd("command_list_begin\n" + enableWebCmd() + "clear\nfindadd AlbumArtist \"" + ea + "\" Album \"" + eb + "\"\nplay 0\ncommand_list_end");
    refreshAll();
  }

  async function addAlbumToQueue(artist, album) {
    await findAdd(artist, album);
    refreshAll();
  }

  async function insertAlbumNext(artist, album) {
    const tracks = await getTracks(artist, album);
    const status = await getStatus();
    let insertPos = parseInt(status.song || "0", 10) + 1;
    for (const t of tracks) {
      await addIdToQueue(t.file, insertPos);
      insertPos++;
    }
    refreshAll();
  }

  async function insertTrackNext(uri) {
    const status = await getStatus();
    const insertPos = parseInt(status.song || "0", 10) + 1;
    await addIdToQueue(uri, insertPos);
    refreshAll();
  }

  // -------------------------------------------------------------------------
  // Ratings UI helpers
  // -------------------------------------------------------------------------

  function renderRatingDots(rating) {
    let html = "";
    for (let i = 1; i <= 10; i++) {
      html += '<span class="star' + (i <= rating ? " filled" : "") + '">\u2605</span>';
    }
    return html;
  }

  function renderClickableRatingDots(rating, prefix) {
    let html = "";
    for (let i = 1; i <= 10; i++) {
      html += '<span class="star clickable' + (i <= rating ? " filled" : "") + '" data-val="' + i + '">\u2605</span>';
    }
    return html;
  }

  function renderRatingStars(id, rating) {
    let html = '<span class="rating-stars" id="' + id + '">';
    for (let i = 1; i <= 10; i++) {
      html += '<span class="star clickable' + (i <= rating ? " filled" : "") + '" data-val="' + i + '">\u2605</span>';
    }
    html += "</span>";
    return html;
  }

  function setupAlbumRatingStars(artist, album, date) {
    const el = document.getElementById("album-rating");
    if (!el) return;
    el.querySelectorAll(".star.clickable").forEach(function (dot) {
      dot.addEventListener("click", function () {
        const val = parseInt(dot.dataset.val, 10);
        rateAlbum(artist, album, date || "", val).then(function () {
          renderTracks(artist, album, date);
        });
      });
    });
  }

  function setupTrackRatingDots() {
    // Track ratings are display-only in the list view for now
    // (click on track = play, not rate)
  }

  function setupNowPlayingRating(songId) {
    const el = document.getElementById("np-rating");
    if (!el) return;
    el.querySelectorAll(".star.clickable").forEach(function (dot) {
      dot.addEventListener("click", function (e) {
        e.stopPropagation();
        const val = parseInt(dot.dataset.val, 10);
        rateTrack(songId, val).then(refreshNowPlaying);
      });
    });
  }

  // -------------------------------------------------------------------------
  // Context Menu
  // -------------------------------------------------------------------------

  const $ctxMenu = document.getElementById("context-menu");

  function showContextMenu(x, y, items) {
    let html = "";
    for (let i = 0; i < items.length; i++) {
      html += '<div class="ctx-item" data-idx="' + i + '">' + esc(items[i].label) + "</div>";
    }
    $ctxMenu.innerHTML = html;
    $ctxMenu.style.left = x + "px";
    $ctxMenu.style.top = y + "px";
    $ctxMenu.classList.remove("hidden");

    $ctxMenu.querySelectorAll(".ctx-item").forEach(function (el) {
      el.addEventListener("click", function () {
        hideContextMenu();
        items[parseInt(el.dataset.idx, 10)].action();
      });
    });
  }

  function hideContextMenu() {
    $ctxMenu.classList.add("hidden");
  }

  document.addEventListener("click", hideContextMenu);
  document.addEventListener("contextmenu", function (e) {
    if (!e.target.closest(".album-card, .track-item, .playlist-item")) {
      hideContextMenu();
    }
  });

  // -------------------------------------------------------------------------
  // Helpers
  // -------------------------------------------------------------------------

  function formatTime(s) {
    s = Math.floor(s || 0);
    const m = Math.floor(s / 60);
    const sec = s % 60;
    return m + ":" + (sec < 10 ? "0" : "") + sec;
  }

  function formatDuration(d) {
    if (!d) return "";
    const s = Math.floor(parseFloat(d));
    return formatTime(s);
  }

  function esc(text) {
    const div = document.createElement("div");
    div.textContent = text || "";
    return div.innerHTML;
  }

  function escAttr(text) {
    return (text || "").replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }

  // -------------------------------------------------------------------------
  // Idle handling & refresh
  // -------------------------------------------------------------------------

  function updateQueueHighlight(pos) {
    // Update main queue view highlight + move ♪ icon
    $content.querySelectorAll(".queue-row.active").forEach(function (el) {
      el.classList.remove("active");
      var posCell = el.querySelector(".qt-pos");
      if (posCell) posCell.textContent = parseInt(el.dataset.pos, 10) + 1;
    });
    var row = $content.querySelector('.queue-row[data-pos="' + pos + '"]');
    if (row) {
      row.classList.add("active");
      var posCell = row.querySelector(".qt-pos");
      if (posCell) posCell.innerHTML = '<span class="now-playing-icon">\u266A</span>';
    }

    // Update queue panel highlight
    var panel = document.getElementById("queue-list");
    if (panel) {
      panel.querySelectorAll(".queue-item.active").forEach(function (el) {
        el.classList.remove("active");
      });
      var item = panel.querySelector('.queue-item[data-pos="' + pos + '"]');
      if (item) item.classList.add("active");
    }
  }

  async function refreshAll() {
    refreshNowPlaying();
    refreshQueuePanel();
    if (currentView === "queue") renderQueue();
  }

  mpd.onIdle = function (changed) {
    for (const sub of changed) {
      switch (sub) {
        case "player":
          refreshNowPlaying();
          getStatus().then(function (st) {
            updateQueueHighlight(parseInt(st.song || "-1", 10));
          });
          break;
        case "playlist":
          refreshQueuePanel();
          if (currentView === "queue") renderQueue();
          break;
        case "mixer":
          refreshNowPlaying();
          break;
        case "database":
          if (currentView === "library" && !libSortLatest) renderLibrary();
          break;
        case "stored_playlist":
          if (currentView === "playlists") renderPlaylists();
          break;
        case "rating":
          refreshNowPlaying();
          if (currentView === "library" && navAlbum) renderTracks(navArtist, navAlbum, navAlbumDate);
          break;
        case "output":
          checkWebActive().then(function () {
            if (!webIsActive && !audio.paused) stopAudio();
          });
          break;
        case "options":
          mpd.cmd("replay_gain_status").then(function (raw) {
            var kv = mpd.parseKV(raw);
            if (kv.replay_gain_mode) {
              rgMode = kv.replay_gain_mode;
              applyReplayGain();
            }
          }).catch(function () {});
          break;
      }
    }
  };

  let webDeviceId = null;
  var webIsActive = false;

  async function checkWebActive() {
    if (!webDeviceId) { webIsActive = false; return; }
    try {
      var raw = await mpd.cmd("outputs");
      var devs = mpd.parseList(raw, "outputid");
      for (var i = 0; i < devs.length; i++) {
        if (devs[i].plugin === "web" && devs[i].outputenabled === "1") {
          webIsActive = true;
          return;
        }
      }
      webIsActive = false;
    } catch (e) { webIsActive = false; }
  }

  mpd.onConnect = async function () {
    // Register as a web playback device
    try {
      const raw = await mpd.cmd("web_register Browser");
      const obj = mpd.parseOne(raw);
      webDeviceId = obj.device_id || null;
    } catch (e) {
      console.error("web_register failed:", e);
    }
    await checkWebActive();
    // Fetch initial ReplayGain mode
    try {
      var rgRaw = await mpd.cmd("replay_gain_status");
      var rgKV = mpd.parseKV(rgRaw);
      if (rgKV.replay_gain_mode) rgMode = rgKV.replay_gain_mode;
    } catch (e) {}
    refreshAll();
    if (currentView === "library") renderLibrary();
  };

  mpd.onDisconnect = function () {
    document.getElementById("np-title").textContent = "Disconnected";
    document.getElementById("np-artist").textContent = "";
    webDeviceId = null;
  };

  // -------------------------------------------------------------------------
  // Init
  // -------------------------------------------------------------------------

  // Try connecting directly — if auth is needed, server returns 401 and we show login
  (async function init() {
    try {
      // Check if we can access the API without auth
      const resp = await fetch("/api/v1/cover/0");
      if (resp.status === 401) {
        // Need auth
        $loginView.style.display = "";
        $loginView.classList.remove("hidden");
        $app.style.display = "none";
        document.getElementById("login-secret").focus();
        return;
      }
    } catch (e) { /* ignore network errors, try connecting anyway */ }

    // No auth needed or already authenticated
    $loginView.style.display = "none";
    $loginView.classList.add("hidden");
    $app.style.display = "";
    mpd.connect();
  })();

})();
