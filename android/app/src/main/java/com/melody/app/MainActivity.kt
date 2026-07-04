package com.melody.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.slideOutVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.offset
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.zIndex
import androidx.compose.ui.input.pointer.changedToUpIgnoreConsumed
import androidx.compose.ui.input.pointer.pointerInput
import kotlinx.coroutines.launch
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.StarHalf
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.PlaylistAdd
import androidx.compose.material.icons.automirrored.filled.PlaylistPlay
import androidx.compose.material.icons.automirrored.filled.QueueMusic
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.foundation.Canvas
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Album
import androidx.compose.material.icons.filled.Casino
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.DeleteSweep
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Devices
import androidx.compose.material.icons.filled.DragHandle
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.DownloadDone
import androidx.compose.material.icons.filled.Equalizer
import androidx.compose.material.icons.filled.FilterList
import androidx.compose.material.icons.filled.FolderOpen
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.LibraryMusic
import androidx.compose.material.icons.filled.LooksOne
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Repeat
import androidx.compose.material.icons.filled.Save
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Schedule
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Shuffle
import androidx.compose.material.icons.filled.SortByAlpha
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.Star
import androidx.compose.material.icons.filled.StarOutline
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import coil3.compose.AsyncImage
import coil3.request.ImageRequest
import coil3.request.crossfade

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        // Request notification permission on Android 13+
        val permsToRequest = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                permsToRequest.add(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
        // Location permission needed to read WiFi SSID for auto server switching
        if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            permsToRequest.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        if (permsToRequest.isNotEmpty()) {
            requestPermissions(permsToRequest.toTypedArray(), 1)
        }
        val serviceIntent = Intent(this, PlaybackService::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(serviceIntent)
        } else {
            startService(serviceIntent)
        }
        setContent {
            MelodyTheme {
                var configured by remember { mutableStateOf(MelodyApp.instance.mpd.isConfigured) }
                if (!configured) {
                    SetupScreen(onConnected = { configured = true })
                } else {
                    val vm: MainViewModel = viewModel()
                    val lifecycle = androidx.lifecycle.compose.LocalLifecycleOwner.current.lifecycle
                    androidx.compose.runtime.DisposableEffect(lifecycle) {
                        val observer = androidx.lifecycle.LifecycleEventObserver { _, event ->
                            if (event == androidx.lifecycle.Lifecycle.Event.ON_RESUME) {
                                // Revive the playback service (and with it the
                                // agent) if it was killed while the app sat
                                // cached in the background.
                                if (PlaybackService.instance == null) {
                                    val svc = Intent(this@MainActivity, PlaybackService::class.java)
                                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                                        startForegroundService(svc)
                                    } else {
                                        startService(svc)
                                    }
                                }
                                vm.onForeground()
                            }
                        }
                        lifecycle.addObserver(observer)
                        onDispose { lifecycle.removeObserver(observer) }
                    }
                    MainScreen(vm)
                }
            }
        }
    }
}

// ==================== Theme ====================

// Melody's own dark palette — used when dynamic color is off or unavailable.
private val melodyDarkScheme = darkColorScheme(
    primary = Color(0xFF60A5FA),
    onPrimary = Color(0xFF0F172A),
    primaryContainer = Color(0xFF1E40AF),
    onPrimaryContainer = Color(0xFFDBEAFE),
    secondary = Color(0xFF94A3B8),
    onSecondary = Color(0xFF0F172A),
    surface = Color(0xFF1E293B),
    onSurface = Color(0xFFF1F5F9),
    surfaceVariant = Color(0xFF334155),
    onSurfaceVariant = Color(0xFF94A3B8),
    surfaceContainerLow = Color(0xFF1E293B),
    surfaceContainer = Color(0xFF1E293B),
    surfaceContainerHigh = Color(0xFF334155),
    background = Color(0xFF0F172A),
    onBackground = Color(0xFFF1F5F9),
    outline = Color(0xFF475569),
)

// Light counterpart of the Melody palette (non-dynamic fallback).
private val melodyLightScheme = androidx.compose.material3.lightColorScheme(
    primary = Color(0xFF2563EB),
    onPrimary = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFDBEAFE),
    onPrimaryContainer = Color(0xFF1E3A8A),
    secondary = Color(0xFF475569),
    surface = Color(0xFFF8FAFC),
    onSurface = Color(0xFF0F172A),
    surfaceVariant = Color(0xFFE2E8F0),
    onSurfaceVariant = Color(0xFF475569),
    surfaceContainerLow = Color(0xFFF1F5F9),
    surfaceContainer = Color(0xFFF1F5F9),
    surfaceContainerHigh = Color(0xFFE2E8F0),
    background = Color(0xFFFFFFFF),
    onBackground = Color(0xFF0F172A),
    outline = Color(0xFF94A3B8),
)

@Composable
fun MelodyTheme(content: @Composable () -> Unit) {
    val dark = when (ThemePrefs.mode) {
        "dark" -> true
        "light" -> false
        else -> androidx.compose.foundation.isSystemInDarkTheme()
    }
    val context = androidx.compose.ui.platform.LocalContext.current
    val scheme = when {
        ThemePrefs.dynamic && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
            if (dark) androidx.compose.material3.dynamicDarkColorScheme(context)
            else androidx.compose.material3.dynamicLightColorScheme(context)
        dark -> melodyDarkScheme
        else -> melodyLightScheme
    }
    MaterialTheme(colorScheme = scheme, content = content)
}

// ==================== Setup ====================

@Composable
fun SetupScreen(onConnected: () -> Unit) {
    var server by remember { mutableStateOf("") }
    var testing by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()

    // Verify the server is actually reachable before committing — otherwise a
    // typo lands the user on an endless "Loading..." screen.
    fun testAndConnect() {
        val trimmed = server.trim()
        if (trimmed.isBlank() || testing) return
        testing = true
        error = null
        scope.launch {
            val ok = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                try {
                    val stripped = trimmed.replace(Regex("^https?://"), "")
                    val lastColon = stripped.lastIndexOf(':')
                    val host = if (lastColon > 0) stripped.substring(0, lastColon) else stripped
                    val port = if (lastColon > 0) stripped.substring(lastColon + 1).toIntOrNull() ?: 6701
                        else if (trimmed.startsWith("https://")) 443 else 6701
                    java.net.Socket().use {
                        it.connect(java.net.InetSocketAddress(host, port), 4000)
                        true
                    }
                } catch (_: Exception) {
                    false
                }
            }
            testing = false
            if (ok) {
                MelodyApp.instance.updateServer(trimmed)
                onConnected()
            } else {
                error = "Could not reach $trimmed"
            }
        }
    }

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = MaterialTheme.colorScheme.background
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .statusBarsPadding()
                .navigationBarsPadding()
                .padding(32.dp),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Icon(
                Icons.Default.MusicNote,
                contentDescription = null,
                modifier = Modifier.size(64.dp),
                tint = MaterialTheme.colorScheme.primary
            )
            Spacer(Modifier.height(16.dp))
            Text(
                "Melody",
                style = MaterialTheme.typography.headlineLarge,
                fontWeight = FontWeight.Bold
            )
            Spacer(Modifier.height(8.dp))
            Text(
                "Connect to your server",
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(Modifier.height(32.dp))
            OutlinedTextField(
                value = server,
                onValueChange = { server = it },
                label = { Text("Server address") },
                placeholder = { Text("192.168.1.10:6701") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
                colors = OutlinedTextFieldDefaults.colors()
            )
            if (error != null) {
                Spacer(Modifier.height(8.dp))
                Text(
                    error!!,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error
                )
            }
            Spacer(Modifier.height(20.dp))
            FilledIconButton(
                onClick = { testAndConnect() },
                enabled = !testing,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(48.dp),
                shape = RoundedCornerShape(12.dp)
            ) {
                Text(
                    if (testing) "Connecting…" else "Connect",
                    style = MaterialTheme.typography.labelLarge
                )
            }
        }
    }
}

// ==================== Main ====================

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(vm: MainViewModel) {
    var selectedTab by androidx.compose.runtime.saveable.rememberSaveable { mutableIntStateOf(0) }
    var showNowPlaying by androidx.compose.runtime.saveable.rememberSaveable { mutableStateOf(false) }
    var showSettings by androidx.compose.runtime.saveable.rememberSaveable { mutableStateOf(false) }
    var showDevices by remember { mutableStateOf(false) }
    var showSaveQueueDialog by remember { mutableStateOf(false) }
    var showClearQueueDialog by remember { mutableStateOf(false) }

    // Tab back-stack: back from any other tab returns to Library instead of
    // closing the app. Declared FIRST so handlers composed later — screen
    // drill-downs, selection mode, and the overlays — take priority over it.
    BackHandler(enabled = selectedTab != 0) { selectedTab = 0 }

    // Transient errors/messages from the ViewModel
    val snackbarHostState = remember { androidx.compose.material3.SnackbarHostState() }
    val toast = vm.toast
    LaunchedEffect(toast) {
        if (toast != null) {
            snackbarHostState.showSnackbar(toast)
            vm.clearToast()
        }
    }

    // Derive top bar title from current state
    val topBarTitle = when (selectedTab) {
        0 -> when (vm.libView) {
            LibView.Artists -> "Library"
            LibView.Albums -> if (vm.libSortLatest) "Latest" else vm.curArtist
            LibView.Tracks -> vm.curAlbum?.album ?: "Tracks"
        }
        1 -> "Search"
        2 -> "Queue"
        3 -> if (vm.playlistView) vm.curPlaylist?.name ?: "Playlists" else "Playlists"
        else -> "Melody"
    }
    val showBackNav = (selectedTab == 0 && vm.libView != LibView.Artists) ||
            (selectedTab == 3 && vm.playlistView)

    Box(Modifier.fillMaxSize()) {
        Scaffold(
            topBar = {
                TopAppBar(
                    title = {
                        Text(
                            topBarTitle,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    },
                    navigationIcon = {
                        if (showBackNav) {
                            IconButton(onClick = {
                                if (selectedTab == 3 && vm.playlistView) vm.playlistBack()
                                else vm.libBack()
                            }) {
                                Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back")
                            }
                        }
                    },
                    actions = {
                        // Tab-specific actions
                        when (selectedTab) {
                            0 -> if (vm.libView == LibView.Artists) {
                                IconButton(onClick = { vm.randomAlbum() }) {
                                    // Dice, not shuffle: this picks a random
                                    // album, it doesn't shuffle anything.
                                    Icon(Icons.Default.Casino, "Random album")
                                }
                            }
                            2 -> if (vm.queue.isNotEmpty()) {
                                IconButton(onClick = { vm.queueShuffle() }) {
                                    Icon(Icons.Default.Shuffle, "Shuffle queue")
                                }
                                IconButton(onClick = { showSaveQueueDialog = true }) {
                                    Icon(Icons.Default.Save, "Save queue as playlist")
                                }
                                IconButton(onClick = { showClearQueueDialog = true }) {
                                    Icon(Icons.Default.DeleteSweep, "Clear queue")
                                }
                            }
                        }
                        // Device chooser in top bar
                        val activeDevice = vm.devices.firstOrNull { it.active }
                        IconButton(onClick = { vm.loadDevices(); showDevices = true }) {
                            Icon(
                                Icons.Default.Devices,
                                "Devices",
                                tint = if (activeDevice != null && !activeDevice.isLocal)
                                    MaterialTheme.colorScheme.primary
                                else MaterialTheme.colorScheme.onSurface
                            )
                        }
                        // Settings always available
                        IconButton(onClick = { showSettings = true }) {
                            Icon(Icons.Default.Settings, "Settings")
                        }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.surface
                    )
                )
            },
            bottomBar = {
                Column {
                    // Download progress
                    val dlProgress = vm.downloadProgress
                    if (dlProgress != null) {
                        Surface(color = MaterialTheme.colorScheme.primaryContainer) {
                            Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp)) {
                                Text(
                                    "Downloading: ${dlProgress.trackTitle} (${dlProgress.current}/${dlProgress.total})",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onPrimaryContainer,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis
                                )
                                Spacer(Modifier.height(4.dp))
                                LinearProgressIndicator(
                                    progress = { dlProgress.current.toFloat() / dlProgress.total },
                                    modifier = Modifier.fillMaxWidth().height(3.dp),
                                    color = MaterialTheme.colorScheme.primary,
                                    trackColor = MaterialTheme.colorScheme.onPrimaryContainer.copy(alpha = 0.2f)
                                )
                            }
                        }
                    }
                    MiniPlayerBar(vm) { showNowPlaying = true }
                    NavigationBar(
                        containerColor = MaterialTheme.colorScheme.surface,
                        tonalElevation = 0.dp
                    ) {
                        NavigationBarItem(
                            selected = selectedTab == 0,
                            onClick = { selectedTab = 0 },
                            icon = { Icon(Icons.Default.LibraryMusic, contentDescription = null) },
                            label = { Text("Library") }
                        )
                        NavigationBarItem(
                            selected = selectedTab == 1,
                            onClick = { selectedTab = 1 },
                            icon = { Icon(Icons.Default.Search, contentDescription = null) },
                            label = { Text("Search") }
                        )
                        NavigationBarItem(
                            selected = selectedTab == 2,
                            onClick = { selectedTab = 2 },
                            icon = { Icon(Icons.AutoMirrored.Filled.QueueMusic, contentDescription = null) },
                            label = { Text("Queue") }
                        )
                        NavigationBarItem(
                            selected = selectedTab == 3,
                            onClick = {
                                // Re-tapping while inside a playlist steps back to
                                // the list; switching tabs keeps the open playlist.
                                if (selectedTab == 3 && vm.playlistView) {
                                    vm.playlistBack()
                                } else {
                                    vm.loadPlaylists(resetView = false)
                                }
                                selectedTab = 3
                            },
                            icon = { Icon(Icons.AutoMirrored.Filled.PlaylistPlay, contentDescription = null) },
                            label = { Text("Playlists") }
                        )
                    }
                }
            },
            snackbarHost = { androidx.compose.material3.SnackbarHost(snackbarHostState) }
        ) { padding ->
            // consumeWindowInsets tells nested imePadding() that the scaffold
            // padding already covers part of the bottom inset — without it the
            // keyboard height and the bottom-bar height stack, leaving a dead
            // band above the keyboard.
            Box(Modifier.padding(padding).consumeWindowInsets(padding)) {
                // Direction-aware slide between tabs, so switching tabs (and
                // the back gesture returning to Library) animates instead of
                // hard-swapping the content.
                AnimatedContent(
                    targetState = selectedTab,
                    transitionSpec = {
                        val forward = targetState > initialState
                        (slideInHorizontally { if (forward) it / 4 else -it / 4 } + fadeIn()) togetherWith
                                (slideOutHorizontally { if (forward) -it / 4 else it / 4 } + fadeOut())
                    },
                    label = "tabs"
                ) { tab ->
                    when (tab) {
                        0 -> LibraryScreen(vm)
                        1 -> SearchScreen(vm)
                        2 -> QueueScreen(vm, onSwitchToLibrary = { selectedTab = 0 })
                        3 -> PlaylistsScreen(vm)
                    }
                }
            }
        }

        // Clear-queue confirmation — as destructive as deleting a playlist,
        // and it sits next to two harmless icons.
        if (showClearQueueDialog) {
            AlertDialog(
                onDismissRequest = { showClearQueueDialog = false },
                title = { Text("Clear queue?") },
                text = { Text("All ${vm.queue.size} tracks will be removed and playback stops.") },
                confirmButton = {
                    TextButton(onClick = {
                        showClearQueueDialog = false
                        vm.queueClear()
                    }) { Text("Clear", color = MaterialTheme.colorScheme.error) }
                },
                dismissButton = {
                    TextButton(onClick = { showClearQueueDialog = false }) { Text("Cancel") }
                }
            )
        }

        // Save-queue-as-playlist dialog
        if (showSaveQueueDialog) {
            var name by remember { mutableStateOf("") }
            AlertDialog(
                onDismissRequest = { showSaveQueueDialog = false },
                title = { Text("Save queue as playlist") },
                text = {
                    OutlinedTextField(
                        value = name,
                        onValueChange = { name = it },
                        placeholder = { Text("Playlist name") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth()
                    )
                },
                confirmButton = {
                    TextButton(
                        onClick = {
                            if (name.isNotBlank()) {
                                vm.saveQueueAsPlaylist(name.trim())
                                showSaveQueueDialog = false
                            }
                        }
                    ) { Text("Save") }
                },
                dismissButton = {
                    TextButton(onClick = { showSaveQueueDialog = false }) { Text("Cancel") }
                }
            )
        }

        // Action menu (queue items have their own sheet)
        if (vm.showActionMenu && vm.actionTarget !is MainViewModel.ActionTarget.QueueItemTarget) {
            ActionSheet(vm)
        }

        // Device chooser bottom sheet
        if (showDevices) {
            DevicesSheet(vm, onDismiss = { showDevices = false })
        }

        // Playlist picker bottom sheet
        if (vm.showPlaylistPicker) {
            PlaylistPickerSheet(vm)
        }

        // "Play on phone?" prompt. Tapping outside cancels the action;
        // "Keep current" explicitly plays on the currently active device.
        if (vm.showPhonePrompt) {
            AlertDialog(
                onDismissRequest = { vm.phonePromptDismiss() },
                title = { Text("Play on phone?") },
                text = { Text("You're on mobile data. Switch playback to this device?") },
                confirmButton = {
                    TextButton(onClick = { vm.phonePromptConfirm() }) { Text("Phone") }
                },
                dismissButton = {
                    TextButton(onClick = { vm.phonePromptPlayOnCurrent() }) { Text("Keep current") }
                }
            )
        }

        // Now Playing overlay
        AnimatedVisibility(
            visible = showNowPlaying,
            enter = slideInVertically { it },
            exit = slideOutVertically { it }
        ) {
            NowPlayingScreen(vm) { showNowPlaying = false }
        }

        // Settings overlay
        AnimatedVisibility(
            visible = showSettings,
            enter = slideInVertically { it },
            exit = slideOutVertically { it }
        ) {
            SettingsScreen(vm, onDismiss = { showSettings = false })
        }
    }
}

// ==================== Mini Player ====================

@Composable
fun MiniPlayerBar(vm: MainViewModel, onClick: () -> Unit) {
    val st = vm.lastPlayingStatus ?: return

    val dur = st.duration
    val pos = st.timePos
    val progress = if (dur > 0) (pos / dur).toFloat().coerceIn(0f, 1f) else 0f

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick),
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        tonalElevation = 0.dp
    ) {
        Column {
            LinearProgressIndicator(
                progress = { progress },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(2.dp),
                color = MaterialTheme.colorScheme.primary,
                trackColor = Color.Transparent
            )
            Row(
                modifier = Modifier.padding(start = 12.dp, end = 4.dp, top = 8.dp, bottom = 8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                // Album art
                val coverUrl = MelodyApp.instance.mpd.coverUrl(st.albumId)
                Box(
                    modifier = Modifier
                        .size(40.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(MaterialTheme.colorScheme.surfaceVariant),
                    contentAlignment = Alignment.Center
                ) {
                    if (coverUrl != null) {
                        AsyncImage(
                            model = ImageRequest.Builder(MelodyApp.instance)
                                .data(coverUrl)
                                .crossfade(true)
                                .build(),
                            contentDescription = "Album art",
                            modifier = Modifier.fillMaxSize(),
                            contentScale = androidx.compose.ui.layout.ContentScale.Crop
                        )
                    } else {
                        Icon(
                            Icons.Default.MusicNote,
                            contentDescription = null,
                            modifier = Modifier.size(20.dp),
                            tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.7f)
                        )
                    }
                }
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (!vm.isConnected) {
                            val infiniteTransition = rememberInfiniteTransition(label = "pulse")
                            val alpha by infiniteTransition.animateFloat(
                                initialValue = 1f,
                                targetValue = 0.2f,
                                animationSpec = infiniteRepeatable(
                                    animation = tween(800),
                                    repeatMode = RepeatMode.Reverse
                                ),
                                label = "pulseAlpha"
                            )
                            Canvas(modifier = Modifier.size(8.dp)) {
                                drawCircle(color = Color(0xFFFF6B35), alpha = alpha)
                            }
                            Spacer(Modifier.width(6.dp))
                        }
                        Text(
                            st.title.ifBlank { "\u2014" },
                            style = MaterialTheme.typography.bodyMedium,
                            fontWeight = FontWeight.Medium,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.weight(1f, fill = false)
                        )
                        if (vm.currentTrackOffline) {
                            Spacer(Modifier.width(4.dp))
                            Icon(
                                Icons.Default.DownloadDone,
                                "Cached",
                                modifier = Modifier.size(14.dp),
                                tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.7f)
                            )
                        }
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (st.artist.isNotBlank()) {
                            Text(
                                st.artist,
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                modifier = Modifier.weight(1f, fill = false)
                            )
                        }
                        if (vm.codecInfo.isNotBlank()) {
                            Spacer(Modifier.width(6.dp))
                            Text(
                                vm.codecInfo,
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1
                            )
                        }
                    }
                }
                IconButton(onClick = { vm.playPrev() }) {
                    Icon(Icons.Default.SkipPrevious, "Previous", modifier = Modifier.size(22.dp))
                }
                IconButton(onClick = { vm.togglePlay() }) {
                    Icon(
                        if (st.state == "playing") Icons.Default.Pause else Icons.Default.PlayArrow,
                        "Play/Pause",
                        modifier = Modifier.size(28.dp)
                    )
                }
                IconButton(onClick = { vm.playNext() }) {
                    Icon(Icons.Default.SkipNext, "Next", modifier = Modifier.size(22.dp))
                }
            }
        }
    }
}

// ==================== Now Playing ====================

@Composable
fun NowPlayingScreen(vm: MainViewModel, onDismiss: () -> Unit) {
    val st = vm.status

    BackHandler { onDismiss() }

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = MaterialTheme.colorScheme.background
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .statusBarsPadding()
                .navigationBarsPadding()
                .padding(horizontal = 28.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // Top bar
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 8.dp, bottom = 4.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                IconButton(onClick = onDismiss) {
                    Icon(
                        Icons.Default.KeyboardArrowDown,
                        "Close",
                        modifier = Modifier.size(32.dp)
                    )
                }
                Spacer(Modifier.weight(1f))
                Text(
                    "Now Playing",
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(Modifier.weight(1f))
                Spacer(Modifier.size(48.dp))
            }

            Spacer(Modifier.weight(0.5f))

            // Album art
            val npCoverUrl = st?.albumId?.let { MelodyApp.instance.mpd.coverUrl(it, 600) }
            Box(
                modifier = Modifier
                    .fillMaxWidth(0.8f)
                    .aspectRatio(1f)
                    .clip(RoundedCornerShape(20.dp))
                    .background(MaterialTheme.colorScheme.surfaceVariant),
                contentAlignment = Alignment.Center
            ) {
                if (npCoverUrl != null) {
                    AsyncImage(
                        model = ImageRequest.Builder(MelodyApp.instance)
                            .data(npCoverUrl)
                            .crossfade(true)
                            .build(),
                        contentDescription = "Album art",
                        modifier = Modifier.fillMaxSize(),
                        contentScale = androidx.compose.ui.layout.ContentScale.Crop
                    )
                } else {
                    Icon(
                        Icons.Default.MusicNote,
                        contentDescription = null,
                        modifier = Modifier.size(72.dp),
                        tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.4f)
                    )
                }
            }

            Spacer(Modifier.height(36.dp))

            // Track info
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    st?.title?.ifBlank { "\u2014" } ?: "Not Playing",
                    style = MaterialTheme.typography.headlineSmall,
                    fontWeight = FontWeight.Bold,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    textAlign = TextAlign.Center
                )
                if (vm.currentTrackOffline) {
                    Spacer(Modifier.width(8.dp))
                    Icon(
                        Icons.Default.DownloadDone,
                        "Cached",
                        modifier = Modifier.size(18.dp),
                        tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.7f)
                    )
                }
            }
            Spacer(Modifier.height(6.dp))
            val sub = listOfNotNull(
                st?.artist?.ifBlank { null },
                st?.album?.ifBlank { null }
            ).joinToString(" \u2014 ")
            Text(
                sub.ifBlank { " " },
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth()
            )

            // Rating stars
            Spacer(Modifier.height(16.dp))
            RatingBar(
                rating = st?.rating ?: 0,
                onRate = { vm.rateCurrentTrack(it) }
            )

            Spacer(Modifier.height(20.dp))

            // Seek bar
            val dur = st?.duration ?: 0.0
            val pos = st?.timePos ?: 0.0
            var dragging by remember { mutableStateOf(false) }
            var dragValue by remember { mutableFloatStateOf(0f) }
            // Holds the just-seeked fraction until the server's status catches
            // up — otherwise the thumb snaps back to the pre-seek position for
            // a second after release.
            var pendingSeek by remember { mutableStateOf<Float?>(null) }
            LaunchedEffect(pos) {
                val p = pendingSeek
                if (p != null && dur > 0 && kotlin.math.abs(pos - p * dur) < 3.0) pendingSeek = null
            }
            LaunchedEffect(pendingSeek) {
                if (pendingSeek != null) {
                    kotlinx.coroutines.delay(3000)
                    pendingSeek = null
                }
            }
            val displayFraction = when {
                dragging -> dragValue
                pendingSeek != null -> pendingSeek!!
                dur > 0 -> (pos / dur).toFloat().coerceIn(0f, 1f)
                else -> 0f
            }

            Column(Modifier.fillMaxWidth()) {
                // Custom seek bar
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(32.dp)
                        .clickable(enabled = false, onClick = {}),
                    contentAlignment = Alignment.CenterStart
                ) {
                    // Track background
                    Box(
                        Modifier
                            .fillMaxWidth()
                            .height(4.dp)
                            .clip(RoundedCornerShape(2.dp))
                            .background(MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
                    )
                    // Track progress
                    Box(
                        Modifier
                            .fillMaxWidth(displayFraction)
                            .height(4.dp)
                            .clip(RoundedCornerShape(2.dp))
                            .background(MaterialTheme.colorScheme.primary)
                    )
                    // Invisible slider on top for interaction
                    Slider(
                        value = displayFraction,
                        onValueChange = { dragging = true; dragValue = it },
                        onValueChangeFinished = {
                            pendingSeek = dragValue
                            vm.seek(dragValue.toDouble() * dur)
                            dragging = false
                        },
                        modifier = Modifier.fillMaxWidth(),
                        enabled = dur > 0,
                        colors = androidx.compose.material3.SliderDefaults.colors(
                            thumbColor = MaterialTheme.colorScheme.primary,
                            activeTrackColor = Color.Transparent,
                            inactiveTrackColor = Color.Transparent,
                            activeTickColor = Color.Transparent,
                            inactiveTickColor = Color.Transparent
                        )
                    )
                }
            }
            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 4.dp)
            ) {
                Text(
                    fmtTime(if (dragging || pendingSeek != null) displayFraction.toDouble() * dur else pos),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(Modifier.weight(1f))
                Text(
                    fmtTime(dur),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            Spacer(Modifier.height(20.dp))

            // Transport controls
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically
            ) {
                IconButton(
                    onClick = { vm.playPrev() },
                    modifier = Modifier.size(64.dp)
                ) {
                    Icon(
                        Icons.Default.SkipPrevious,
                        "Previous",
                        modifier = Modifier.size(36.dp)
                    )
                }
                Spacer(Modifier.width(20.dp))
                FilledIconButton(
                    onClick = { vm.togglePlay() },
                    modifier = Modifier.size(72.dp),
                    shape = CircleShape
                ) {
                    Icon(
                        if (st?.state == "playing") Icons.Default.Pause else Icons.Default.PlayArrow,
                        "Play/Pause",
                        modifier = Modifier.size(40.dp)
                    )
                }
                Spacer(Modifier.width(20.dp))
                IconButton(
                    onClick = { vm.playNext() },
                    modifier = Modifier.size(64.dp)
                ) {
                    Icon(
                        Icons.Default.SkipNext,
                        "Next",
                        modifier = Modifier.size(36.dp)
                    )
                }
            }

            Spacer(Modifier.height(24.dp))

            // Mode buttons
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceEvenly,
                verticalAlignment = Alignment.CenterVertically
            ) {
                ModeIconButton(Icons.Default.Repeat, "Repeat", active = st?.repeat == true) { vm.toggleRepeat() }
                ModeIconButton(Icons.Default.Shuffle, "Random", active = st?.random == true) { vm.toggleRandom() }
                ModeIconButton(Icons.Default.LooksOne, "Single", active = st?.single == true) { vm.toggleSingle() }
                PacManButton(active = st?.consume == true) { vm.toggleConsume() }
            }

            // Volume slider (server reports -1 when the target has no volume
            // control). Styled like the seek bar above — thin custom track
            // with an invisible Slider for interaction.
            val vol = st?.volume ?: -1
            if (vol >= 0) {
                Spacer(Modifier.height(8.dp))
                var volDrag by remember { mutableStateOf<Float?>(null) }
                val volFraction = volDrag ?: (vol / 100f).coerceIn(0f, 1f)
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(
                        Icons.AutoMirrored.Filled.VolumeUp,
                        contentDescription = "Volume",
                        modifier = Modifier.size(20.dp),
                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Spacer(Modifier.width(12.dp))
                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .height(32.dp),
                        contentAlignment = Alignment.CenterStart
                    ) {
                        Box(
                            Modifier
                                .fillMaxWidth()
                                .height(4.dp)
                                .clip(RoundedCornerShape(2.dp))
                                .background(MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
                        )
                        Box(
                            Modifier
                                .fillMaxWidth(volFraction)
                                .height(4.dp)
                                .clip(RoundedCornerShape(2.dp))
                                .background(MaterialTheme.colorScheme.primary)
                        )
                        Slider(
                            value = volFraction,
                            onValueChange = { volDrag = it },
                            onValueChangeFinished = {
                                volDrag?.let { vm.setVolume((it * 100).toInt()) }
                                volDrag = null
                            },
                            modifier = Modifier.fillMaxWidth(),
                            colors = androidx.compose.material3.SliderDefaults.colors(
                                thumbColor = MaterialTheme.colorScheme.primary,
                                activeTrackColor = Color.Transparent,
                                inactiveTrackColor = Color.Transparent,
                                activeTickColor = Color.Transparent,
                                inactiveTickColor = Color.Transparent
                            )
                        )
                    }
                    Spacer(Modifier.width(12.dp))
                    Text(
                        "${(volFraction * 100).toInt()}",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.width(28.dp)
                    )
                }
            }

            Spacer(Modifier.weight(1f))
        }
    }

    // Lyrics overlay
    var showLyrics by remember { mutableStateOf(false) }

    // Lyrics button — floating at bottom-right of now playing
    if (vm.lyrics != null) {
        Box(modifier = Modifier.fillMaxSize()) {
            IconButton(
                onClick = { showLyrics = true },
                modifier = Modifier
                    .align(Alignment.BottomEnd)
                    .padding(end = 24.dp, bottom = 24.dp)
                    .navigationBarsPadding()
            ) {
                Icon(
                    Icons.AutoMirrored.Filled.QueueMusic,
                    "Lyrics",
                    modifier = Modifier.size(28.dp),
                    tint = MaterialTheme.colorScheme.primary
                )
            }
        }
    }

    AnimatedVisibility(
        visible = showLyrics,
        enter = slideInVertically(initialOffsetY = { it }),
        exit = slideOutVertically(targetOffsetY = { it })
    ) {
        LyricsScreen(vm) { showLyrics = false }
    }
}

@Composable
fun LyricsScreen(vm: MainViewModel, onDismiss: () -> Unit) {
    val lyr = vm.lyrics
    if (lyr == null) {
        // Dismiss in an effect — mutating state during composition forces an
        // immediate extra recomposition.
        LaunchedEffect(Unit) { onDismiss() }
        return
    }
    val st = vm.status

    BackHandler { onDismiss() }

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = MaterialTheme.colorScheme.background
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .statusBarsPadding()
                .navigationBarsPadding()
        ) {
            // Top bar
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 8.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                IconButton(onClick = onDismiss) {
                    Icon(Icons.Default.KeyboardArrowDown, "Close", modifier = Modifier.size(32.dp))
                }
                Spacer(Modifier.weight(1f))
                Text(
                    "Lyrics",
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(Modifier.weight(1f))
                Spacer(Modifier.size(48.dp))
            }

            // Lyrics content
            val lines = remember(lyr.text) { parseLyricsLines(lyr.text, lyr.type) }
            val listState = rememberLazyListState()
            val elapsed = st?.timePos ?: 0.0

            // Auto-scroll for synced lyrics
            if (lyr.type == "synced" && lines.isNotEmpty()) {
                val activeLine = lines.indexOfLast { it.time >= 0 && it.time <= elapsed }
                LaunchedEffect(activeLine) {
                    if (activeLine > 0) {
                        listState.animateScrollToItem((activeLine - 2).coerceAtLeast(0))
                    }
                }
            }

            LazyColumn(
                state = listState,
                modifier = Modifier
                    .fillMaxSize()
                    .padding(horizontal = 24.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                items(lines.size) { i ->
                    val line = lines[i]
                    val isActive = if (lyr.type == "synced") {
                        val nextTime = lines.getOrNull(i + 1)?.time ?: Double.MAX_VALUE
                        line.time >= 0 && elapsed >= line.time && elapsed < nextTime
                    } else false

                    Text(
                        line.text,
                        style = MaterialTheme.typography.bodyLarge,
                        fontWeight = if (isActive) FontWeight.Bold else FontWeight.Normal,
                        color = if (isActive) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
                        modifier = Modifier.padding(vertical = 4.dp)
                    )
                }
            }
        }
    }
}

private data class LyricsLine(val time: Double, val text: String)

private fun parseLyricsLines(text: String, type: String): List<LyricsLine> {
    return text.lines().mapNotNull { line ->
        if (line.isBlank()) return@mapNotNull null
        if (type == "synced") {
            // Parse [mm:ss.xx] prefix
            val match = Regex("""\[(\d+):(\d+)\.(\d+)](.*)""").find(line)
            if (match != null) {
                val (min, sec, hundredths) = match.destructured
                val time = min.toDouble() * 60 + sec.toDouble() + hundredths.toDouble() / 100.0
                LyricsLine(time, match.groupValues[4].trim())
            } else {
                LyricsLine(-1.0, line)
            }
        } else {
            LyricsLine(-1.0, line)
        }
    }.filter { it.text.isNotBlank() }
}

@Composable
fun ModeIconButton(icon: androidx.compose.ui.graphics.vector.ImageVector, description: String, active: Boolean, onClick: () -> Unit) {
    // Active = primary (clearly "on"); inactive stays readable instead of
    // looking disabled.
    val tint = if (active) MaterialTheme.colorScheme.primary
               else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
    IconButton(onClick = onClick, modifier = Modifier.size(44.dp)) {
        Icon(icon, contentDescription = description, modifier = Modifier.size(24.dp), tint = tint)
    }
}

@Composable
fun PacManButton(active: Boolean, onClick: () -> Unit) {
    val color = if (active) MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
    IconButton(onClick = onClick, modifier = Modifier.size(44.dp)) {
        Canvas(modifier = Modifier.size(24.dp)) {
            val r = size.minDimension / 2f
            drawArc(
                color = color,
                startAngle = 35f,
                sweepAngle = 290f,
                useCenter = true,
                topLeft = androidx.compose.ui.geometry.Offset.Zero,
                size = size
            )
        }
    }
}

// ==================== Library ====================

@Composable
fun LibraryScreen(vm: MainViewModel) {
    BackHandler(enabled = vm.libView != LibView.Artists || vm.libSortLatest) {
        vm.libBack()
    }

    AnimatedContent(
        targetState = vm.libView,
        transitionSpec = {
            val forward = targetState.ordinal > initialState.ordinal
            (slideInHorizontally { if (forward) it else -it } + fadeIn()) togetherWith
                    (slideOutHorizontally { if (forward) -it else it } + fadeOut())
        },
        label = "library"
    ) { view ->
        when (view) {
            LibView.Artists -> ArtistList(vm)
            LibView.Albums -> AlbumList(vm)
            LibView.Tracks -> TrackList(vm)
        }
    }
}

@Composable
fun Scrollbar(
    listState: androidx.compose.foundation.lazy.LazyListState,
    modifier: Modifier = Modifier,
    onDragging: ((Boolean) -> Unit)? = null
) {
    val info = listState.layoutInfo
    val totalItems = info.totalItemsCount
    if (totalItems == 0) return
    val visibleCount = info.visibleItemsInfo.size
    if (visibleCount >= totalItems) return

    val scope = rememberCoroutineScope()
    var dragging by remember { mutableStateOf(false) }

    val thumbFraction = (visibleCount.toFloat() / totalItems).coerceIn(0.05f, 1f)
    val scrollFraction = listState.firstVisibleItemIndex.toFloat() / (totalItems - visibleCount).coerceAtLeast(1)

    val thumbColor = MaterialTheme.colorScheme.onSurface.copy(
        alpha = if (dragging) 0.6f else if (listState.isScrollInProgress) 0.4f else 0.15f
    )
    val trackWidth = if (dragging) 8.dp else 4.dp

    Box(
        modifier = modifier
            .fillMaxHeight()
            .width(24.dp) // wide touch target
            .padding(vertical = 4.dp)
            .pointerInput(totalItems) {
                awaitPointerEventScope {
                    while (true) {
                        val down = awaitFirstDown(requireUnconsumed = false)
                        dragging = true
                        onDragging?.invoke(true)
                        fun scrollTo(y: Float) {
                            val fraction = (y / size.height).coerceIn(0f, 1f)
                            val targetItem = (fraction * (totalItems - 1)).toInt()
                            scope.launch { listState.scrollToItem(targetItem) }
                        }
                        scrollTo(down.position.y)
                        down.consume()
                        while (true) {
                            val event = awaitPointerEvent()
                            val change = event.changes.firstOrNull() ?: break
                            if (change.changedToUpIgnoreConsumed()) {
                                dragging = false
                                onDragging?.invoke(false)
                                change.consume()
                                break
                            }
                            scrollTo(change.position.y)
                            change.consume()
                        }
                    }
                }
            },
        contentAlignment = Alignment.TopEnd
    ) {
        androidx.compose.foundation.Canvas(
            modifier = Modifier
                .fillMaxHeight()
                .width(trackWidth)
        ) {
            val trackH = size.height
            val thumbH = (trackH * thumbFraction).coerceAtLeast(24.dp.toPx())
            val maxOffset = trackH - thumbH
            val thumbY = scrollFraction * maxOffset

            drawRoundRect(
                color = thumbColor,
                topLeft = androidx.compose.ui.geometry.Offset(0f, thumbY),
                size = androidx.compose.ui.geometry.Size(size.width, thumbH),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(size.width / 2f)
            )
        }
    }
}

@Composable
fun ArtistList(vm: MainViewModel) {
    if (vm.artists.isEmpty() && !vm.showCachedOnly) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                if (vm.isConnected) {
                    androidx.compose.material3.CircularProgressIndicator(modifier = Modifier.size(36.dp))
                    Spacer(Modifier.height(16.dp))
                    Text("Loading library…", color = MaterialTheme.colorScheme.onSurfaceVariant)
                } else {
                    Icon(
                        Icons.Default.LibraryMusic,
                        contentDescription = null,
                        modifier = Modifier.size(48.dp),
                        tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
                    )
                    Spacer(Modifier.height(12.dp))
                    Text("Can't reach the server", color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Spacer(Modifier.height(12.dp))
                    TextButton(onClick = { vm.onForeground() }) {
                        Icon(Icons.Default.Refresh, null, modifier = Modifier.size(18.dp))
                        Spacer(Modifier.width(6.dp))
                        Text("Retry")
                    }
                }
            }
        }
        return
    }

    val listState = rememberLazyListState(
        initialFirstVisibleItemIndex = vm.savedArtistScrollIndex,
        initialFirstVisibleItemScrollOffset = vm.savedArtistScrollOffset
    )

    Box(Modifier.fillMaxSize()) {
        LazyColumn(state = listState, modifier = Modifier.fillMaxSize()) {
            item {
                Row(
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        if (vm.showCachedOnly && vm.artists.isEmpty()) "No offline albums"
                        else "${vm.artists.size} artists",
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    // Labeled chips instead of cryptic icon toggles
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FilterChip(
                            selected = vm.libSortLatest,
                            onClick = { vm.toggleLibSortLatest() },
                            label = { Text("Latest") },
                            leadingIcon = {
                                Icon(Icons.Default.Schedule, null, modifier = Modifier.size(16.dp))
                            }
                        )
                        FilterChip(
                            selected = vm.showCachedOnly,
                            onClick = { vm.toggleCachedOnly() },
                            label = { Text("Downloaded") },
                            leadingIcon = {
                                Icon(Icons.Default.DownloadDone, null, modifier = Modifier.size(16.dp))
                            }
                        )
                    }
                }
            }
            itemsIndexed(vm.artists) { _, artist ->
                ListItem(
                    headlineContent = {
                        Text(artist, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    },
                    modifier = Modifier.clickable {
                        vm.saveArtistScroll(listState.firstVisibleItemIndex, listState.firstVisibleItemScrollOffset)
                        vm.loadAlbums(artist)
                    },
                    trailingContent = {
                        IconButton(onClick = { vm.showAction(MainViewModel.ActionTarget.ArtistTarget(artist)) }) {
                            Icon(Icons.Default.MoreVert, "Actions")
                        }
                    }
                )
            }
        }

        // Scroll letter indicator
        var scrollbarDragging by remember { mutableStateOf(false) }
        val isScrolling = listState.isScrollInProgress || scrollbarDragging
        val firstIdx = listState.firstVisibleItemIndex
        val currentLetter = if (firstIdx > 0 && firstIdx <= vm.artists.size) {
            vm.artists[firstIdx - 1].firstOrNull()?.uppercase() ?: ""
        } else ""
        AnimatedVisibility(
            visible = isScrolling && currentLetter.isNotEmpty(),
            enter = fadeIn(), exit = fadeOut(),
            modifier = Modifier.align(Alignment.CenterEnd).padding(end = 48.dp).zIndex(10f)
        ) {
            Surface(shape = RoundedCornerShape(8.dp), color = MaterialTheme.colorScheme.primaryContainer, tonalElevation = 8.dp) {
                Text(currentLetter, modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
                    style = MaterialTheme.typography.headlineMedium, color = MaterialTheme.colorScheme.onPrimaryContainer)
            }
        }

        Scrollbar(listState, Modifier.align(Alignment.CenterEnd), onDragging = { scrollbarDragging = it })
    }
}

@Composable
fun AlbumList(vm: MainViewModel) {
    val listState = rememberLazyListState(
        initialFirstVisibleItemIndex = vm.savedAlbumScrollIndex,
        initialFirstVisibleItemScrollOffset = vm.savedAlbumScrollOffset
    )

    Box(Modifier.fillMaxSize()) {
        LazyColumn(state = listState, modifier = Modifier.fillMaxSize()) {
            item {
                Text(
                    "${vm.albums.size} albums",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
                )
            }
            items(vm.albums, key = { it.id.ifBlank { "${it.albumArtist}\u0000${it.album}\u0000${it.date}" } }) { album ->
                val isOffline = vm.downloadedAlbums.contains(album.id)
                ListItem(
                    leadingContent = {
                        val albumCoverUrl = MelodyApp.instance.mpd.coverUrl(album.id, 150)
                        Box(
                            modifier = Modifier
                                .size(48.dp)
                                .clip(RoundedCornerShape(6.dp))
                                .background(MaterialTheme.colorScheme.surfaceVariant),
                            contentAlignment = Alignment.Center
                        ) {
                            if (albumCoverUrl != null) {
                                AsyncImage(
                                    model = ImageRequest.Builder(MelodyApp.instance)
                                        .data(albumCoverUrl)
                                        .crossfade(true)
                                        .build(),
                                    contentDescription = "Album art",
                                    modifier = Modifier.fillMaxSize(),
                                    contentScale = androidx.compose.ui.layout.ContentScale.Crop
                                )
                            } else {
                                Icon(Icons.Default.MusicNote, null, modifier = Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    },
                    headlineContent = {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(album.album, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                            if (isOffline) {
                                Spacer(Modifier.width(6.dp))
                                Icon(
                                    Icons.Default.DownloadDone,
                                    "Downloaded",
                                    modifier = Modifier.size(16.dp),
                                    tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.7f)
                                )
                            }
                        }
                    },
                    supportingContent = {
                        val parts = mutableListOf<String>()
                        if (vm.libSortLatest && album.albumArtist.isNotBlank()) parts.add(album.albumArtist)
                        if (album.date.isNotBlank() && album.date != "0000") parts.add(album.date)
                        if (parts.isNotEmpty()) Text(parts.joinToString(" \u00B7 "))
                    },
                    modifier = Modifier.clickable {
                        vm.saveAlbumScroll(listState.firstVisibleItemIndex, listState.firstVisibleItemScrollOffset)
                        vm.loadTracks(album)
                    },
                    trailingContent = {
                        IconButton(onClick = { vm.showAction(MainViewModel.ActionTarget.AlbumTarget(album)) }) {
                            Icon(Icons.Default.MoreVert, "Actions")
                        }
                    }
                )
            }
        }

        // Scroll letter indicator
        var scrollbarDragging by remember { mutableStateOf(false) }
        val isScrolling = listState.isScrollInProgress || scrollbarDragging
        val firstIdx = listState.firstVisibleItemIndex
        val currentLetter = if (firstIdx > 0 && firstIdx <= vm.albums.size) {
            vm.albums[firstIdx - 1].album.firstOrNull()?.uppercase() ?: ""
        } else ""
        AnimatedVisibility(
            visible = isScrolling && currentLetter.isNotEmpty(),
            enter = fadeIn(), exit = fadeOut(),
            modifier = Modifier.align(Alignment.CenterEnd).padding(end = 48.dp).zIndex(10f)
        ) {
            Surface(shape = RoundedCornerShape(8.dp), color = MaterialTheme.colorScheme.primaryContainer, tonalElevation = 8.dp) {
                Text(currentLetter, modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
                    style = MaterialTheme.typography.headlineMedium, color = MaterialTheme.colorScheme.onPrimaryContainer)
            }
        }

        Scrollbar(listState, Modifier.align(Alignment.CenterEnd), onDragging = { scrollbarDragging = it })
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun TrackList(vm: MainViewModel) {
    LazyColumn(Modifier.fillMaxSize()) {
        // Album header
        if (vm.curAlbum != null) {
            item {
                Row(
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    val headerCoverUrl = MelodyApp.instance.mpd.coverUrl(vm.curAlbum!!.id, 300)
                    Box(
                        modifier = Modifier
                            .size(80.dp)
                            .clip(RoundedCornerShape(10.dp))
                            .background(MaterialTheme.colorScheme.surfaceVariant),
                        contentAlignment = Alignment.Center
                    ) {
                        if (headerCoverUrl != null) {
                            AsyncImage(
                                model = ImageRequest.Builder(MelodyApp.instance)
                                    .data(headerCoverUrl)
                                    .crossfade(true)
                                    .build(),
                                contentDescription = "Album art",
                                modifier = Modifier.fillMaxSize(),
                                contentScale = androidx.compose.ui.layout.ContentScale.Crop
                            )
                        } else {
                            Icon(Icons.Default.MusicNote, null, modifier = Modifier.size(32.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    Spacer(Modifier.width(16.dp))
                    Column {
                        Text(
                            vm.curArtist,
                            style = MaterialTheme.typography.labelLarge,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                        if (vm.curAlbum?.date?.isNotBlank() == true && vm.curAlbum?.date != "0000") {
                            Text(
                                vm.curAlbum!!.date,
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
                            )
                        }
                        Spacer(Modifier.height(4.dp))
                        Text(
                            "${vm.tracks.size} tracks",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
                        )
                        Spacer(Modifier.height(8.dp))
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            FilledIconButton(
                                onClick = { vm.curAlbum?.let { vm.playAlbum(it) } },
                                modifier = Modifier.size(40.dp),
                                shape = CircleShape
                            ) {
                                Icon(Icons.Default.PlayArrow, "Play album", modifier = Modifier.size(24.dp))
                            }
                            IconButton(
                                onClick = { vm.curAlbum?.let { vm.playAlbumShuffled(it) } },
                                modifier = Modifier.size(40.dp)
                            ) {
                                Icon(Icons.Default.Shuffle, "Shuffle album", modifier = Modifier.size(22.dp))
                            }
                        }
                        val displayRating = if (vm.albumRating > 0) vm.albumRating
                            else vm.albumComputedRating.let { if (it > 0.0) kotlin.math.round(it).toInt().coerceIn(1, 10) else 0 }
                        val isComputed = vm.albumRating == 0 && displayRating > 0
                        Spacer(Modifier.height(6.dp))
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            StarRating(
                                rating = displayRating,
                                boxSize = 26.dp,
                                iconSize = 22.dp,
                                computed = isComputed,
                                // Cycle from the user's own rating, not the
                                // computed average shown when unrated.
                                cycleFrom = vm.albumRating,
                                onRate = { vm.rateAlbum(it) }
                            )
                        }
                    }
                }
                HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = 0.3f))
            }
        }
        itemsIndexed(vm.tracks) { idx, track ->
            ListItem(
                leadingContent = {
                    Text(
                        "${track.trackNumber}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.width(28.dp),
                        textAlign = TextAlign.End
                    )
                },
                headlineContent = {
                    Text(track.title, maxLines = 1, overflow = TextOverflow.Ellipsis)
                },
                // Only show the artist when it differs from the album artist
                // (compilations) — on single-artist albums it's noise.
                supportingContent = if (track.artist.isNotBlank() && track.artist != vm.curArtist) {{
                    Text(track.artist, maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodySmall)
                }} else null,
                trailingContent = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (track.duration > 0) {
                            Text(
                                fmtTime(track.duration),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                        IconButton(onClick = { vm.showAction(MainViewModel.ActionTarget.TrackTarget(track)) }) {
                            Icon(Icons.Default.MoreVert, "Actions")
                        }
                    }
                },
                // Tap = play the album from this track; menu via ⋮ or long-press
                modifier = Modifier.combinedClickable(
                    onClick = { vm.playTrackInContext(vm.tracks, idx) },
                    onLongClick = { vm.showAction(MainViewModel.ActionTarget.TrackTarget(track)) }
                )
            )
        }
    }
}

// ==================== Search ====================

@Composable
@OptIn(ExperimentalFoundationApi::class)
fun SearchScreen(vm: MainViewModel) {
    // Back exits selection mode instead of leaving the app
    BackHandler(enabled = vm.searchSelectionMode) { vm.exitSearchSelectionMode() }
    val focusManager = androidx.compose.ui.platform.LocalFocusManager.current

    Box(Modifier.fillMaxSize().imePadding()) {
        Column(Modifier.fillMaxSize()) {
            OutlinedTextField(
                value = vm.searchQuery,
                onValueChange = { vm.updateSearch(it) },
                placeholder = { Text("Search albums and tracks\u2026 (artist:/album:/title:/date:)") },
                singleLine = true,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                shape = RoundedCornerShape(16.dp),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                keyboardActions = KeyboardActions(onSearch = { focusManager.clearFocus() }),
                leadingIcon = { Icon(Icons.Default.Search, "Search") },
                trailingIcon = {
                    if (vm.searchQuery.isNotBlank()) {
                        IconButton(onClick = { vm.updateSearch("") }) {
                            Icon(Icons.Default.Clear, "Clear")
                        }
                    }
                }
            )

            // Rating filter row
            RatingFilterRow(vm)

            val res = vm.searchResult
            val selMode = vm.searchSelectionMode
            val bottomPad = if (selMode && vm.searchSelectionCount > 0) 64.dp else 0.dp
            val searchListState = rememberLazyListState()

            // Build flat list of labels for scroll indicator
            val scrollLabels = remember(res) {
                val labels = mutableListOf<String>()
                if (res.albums.isNotEmpty()) {
                    labels.add("") // "Albums" header
                    for (a in res.albums) labels.add(a.albumArtist.firstOrNull()?.uppercase() ?: "")
                }
                if (res.tracks.isNotEmpty()) {
                    labels.add("") // "Tracks" header
                    for (t in res.tracks) labels.add(t.artist.firstOrNull()?.uppercase() ?: "")
                }
                if (res.albums.isEmpty() && res.tracks.isEmpty() && vm.searchQuery.isNotBlank()) {
                    labels.add("")
                }
                labels
            }

            Box(Modifier.fillMaxSize()) {
                // Scroll letter indicator
                val isScrolling = searchListState.isScrollInProgress
                val firstIdx = searchListState.firstVisibleItemIndex
                val currentLetter = scrollLabels.getOrElse(firstIdx) { "" }
                androidx.compose.animation.AnimatedVisibility(
                    visible = isScrolling && currentLetter.isNotEmpty(),
                    enter = fadeIn(),
                    exit = fadeOut(),
                    modifier = Modifier.align(Alignment.CenterEnd).padding(end = 16.dp).zIndex(10f)
                ) {
                    Surface(
                        shape = RoundedCornerShape(8.dp),
                        color = MaterialTheme.colorScheme.primaryContainer,
                        tonalElevation = 8.dp
                    ) {
                        Text(
                            currentLetter,
                            modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
                            style = MaterialTheme.typography.headlineMedium,
                            color = MaterialTheme.colorScheme.onPrimaryContainer
                        )
                    }
                }

            LazyColumn(state = searchListState, modifier = Modifier.fillMaxSize().padding(bottom = bottomPad)) {
                if (res.albums.isNotEmpty()) {
                    item {
                        Row(
                            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("Albums", style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.primary)
                            if (selMode) {
                                val allSelected = vm.selectedSearchAlbums.size == res.albums.size && res.albums.isNotEmpty()
                                TextButton(onClick = { if (allSelected) vm.deselectAllSearchAlbums() else vm.selectAllSearchAlbums() }) {
                                    Text(if (allSelected) "Deselect all" else "Select all", style = MaterialTheme.typography.labelSmall)
                                }
                            }
                        }
                    }
                    itemsIndexed(res.albums) { _, album ->
                        val selected = album in vm.selectedSearchAlbums
                        ListItem(
                            colors = ListItemDefaults.colors(
                                containerColor = if (selected) MaterialTheme.colorScheme.primaryContainer
                                    else MaterialTheme.colorScheme.surface
                            ),
                            leadingContent = {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    if (selMode) {
                                        Checkbox(checked = selected, onCheckedChange = { vm.toggleSearchAlbum(album) })
                                    }
                                    val searchAlbumCoverUrl = MelodyApp.instance.mpd.coverUrl(album.id, 150)
                                    Box(
                                        modifier = Modifier
                                            .size(48.dp)
                                            .clip(RoundedCornerShape(6.dp))
                                            .background(MaterialTheme.colorScheme.surfaceVariant),
                                        contentAlignment = Alignment.Center
                                    ) {
                                        if (searchAlbumCoverUrl != null) {
                                            AsyncImage(
                                                model = ImageRequest.Builder(MelodyApp.instance)
                                                    .data(searchAlbumCoverUrl)
                                                    .crossfade(true)
                                                    .build(),
                                                contentDescription = "Album art",
                                                modifier = Modifier.fillMaxSize(),
                                                contentScale = androidx.compose.ui.layout.ContentScale.Crop
                                            )
                                        } else {
                                            Icon(Icons.Default.MusicNote, null, modifier = Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
                                        }
                                    }
                                }
                            },
                            headlineContent = {
                                Text(album.album, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            },
                            supportingContent = {
                                val parts = mutableListOf(album.albumArtist)
                                if (album.date.isNotBlank()) parts.add(album.date)
                                Text(parts.joinToString(" \u2022 "), maxLines = 1, overflow = TextOverflow.Ellipsis)
                            },
                            modifier = Modifier.combinedClickable(
                                onClick = {
                                    if (selMode) vm.toggleSearchAlbum(album)
                                    else vm.showAction(MainViewModel.ActionTarget.SearchAlbumTarget(album))
                                },
                                onLongClick = {
                                    if (!selMode) vm.enterSearchSelectionMode(album = album)
                                    else vm.toggleSearchAlbum(album)
                                }
                            )
                        )
                    }
                }
                if (res.tracks.isNotEmpty()) {
                    item {
                        Row(
                            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("Tracks", style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.primary)
                            if (selMode) {
                                val allSelected = vm.selectedSearchTracks.size == res.tracks.size && res.tracks.isNotEmpty()
                                TextButton(onClick = { if (allSelected) vm.deselectAllSearchTracks() else vm.selectAllSearchTracks() }) {
                                    Text(if (allSelected) "Deselect all" else "Select all", style = MaterialTheme.typography.labelSmall)
                                }
                            }
                        }
                    }
                    itemsIndexed(res.tracks) { _, track ->
                        val selected = track.uri in vm.selectedSearchTracks
                        ListItem(
                            colors = ListItemDefaults.colors(
                                containerColor = if (selected) MaterialTheme.colorScheme.primaryContainer
                                    else MaterialTheme.colorScheme.surface
                            ),
                            leadingContent = if (selMode) {{
                                Checkbox(checked = selected, onCheckedChange = { vm.toggleSearchTrack(track.uri) })
                            }} else null,
                            headlineContent = {
                                Text(track.title, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            },
                            supportingContent = {
                                Text(
                                    "${track.artist} \u2014 ${track.album}",
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis
                                )
                            },
                            modifier = Modifier.combinedClickable(
                                onClick = {
                                    if (selMode) vm.toggleSearchTrack(track.uri)
                                    else vm.showAction(MainViewModel.ActionTarget.SearchTrackTarget(track))
                                },
                                onLongClick = {
                                    if (!selMode) vm.enterSearchSelectionMode(trackUri = track.uri)
                                    else vm.toggleSearchTrack(track.uri)
                                }
                            )
                        )
                    }
                }
                if (vm.searchQuery.isNotBlank() && res.albums.isEmpty() && res.tracks.isEmpty()) {
                    item {
                        Box(
                            Modifier
                                .fillMaxWidth()
                                .padding(48.dp),
                            contentAlignment = Alignment.Center
                        ) {
                            Text(
                                "No results found",
                                style = MaterialTheme.typography.bodyLarge,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                }
            }
            } // end scroll indicator Box
        }

        // Batch action bar
        if (vm.searchSelectionMode && vm.searchSelectionCount > 0) {
            Surface(
                modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth(),
                color = MaterialTheme.colorScheme.surfaceContainerHigh,
                tonalElevation = 4.dp
            ) {
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    IconButton(onClick = { vm.exitSearchSelectionMode() }) {
                        Icon(Icons.Default.Close, "Cancel")
                    }
                    Text(
                        "${vm.searchSelectionCount} selected",
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.weight(1f).padding(start = 4.dp)
                    )
                    TextButton(onClick = { vm.executeBatchAction("add") }) { Text("Add") }
                    TextButton(onClick = { vm.executeBatchAction("insert") }) { Text("Insert") }
                    TextButton(onClick = { vm.executeBatchAction("replace") }) { Text("Replace") }
                }
            }
        }
    }
}

@Composable
fun RatingFilterRow(vm: MainViewModel) {
    var expanded by remember { mutableStateOf(false) }
    val hasFilter = vm.searchRatingValue != null

    Column(Modifier.padding(horizontal = 16.dp)) {
        FilterChip(
            selected = hasFilter,
            onClick = {
                if (hasFilter && !expanded) {
                    vm.clearRatingFilter()
                } else {
                    expanded = !expanded
                }
            },
            label = {
                if (hasFilter) {
                    val typeLabel = if (vm.searchRatingType == "albumrating") "Album" else "Track"
                    Text("$typeLabel ${vm.searchRatingOp} ${vm.searchRatingValue}")
                } else {
                    Text("Rating filter")
                }
            },
            trailingIcon = if (hasFilter) {{
                Icon(Icons.Default.Close, "Clear filter",
                    modifier = Modifier.size(16.dp).clickable { vm.clearRatingFilter(); expanded = false })
            }} else null
        )

        if (expanded) {
            Row(
                Modifier.fillMaxWidth().padding(top = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                // Type dropdown
                var typeMenuOpen by remember { mutableStateOf(false) }
                Box {
                    TextButton(onClick = { typeMenuOpen = true }) {
                        Text(if (vm.searchRatingType == "albumrating") "Album" else "Track")
                        Icon(Icons.Default.ArrowDropDown, null, modifier = Modifier.size(18.dp))
                    }
                    DropdownMenu(expanded = typeMenuOpen, onDismissRequest = { typeMenuOpen = false }) {
                        DropdownMenuItem(text = { Text("Track") }, onClick = {
                            vm.setRatingFilter("rating", vm.searchRatingOp, vm.searchRatingValue ?: 5)
                            typeMenuOpen = false
                        })
                        DropdownMenuItem(text = { Text("Album") }, onClick = {
                            vm.setRatingFilter("albumrating", vm.searchRatingOp, vm.searchRatingValue ?: 5)
                            typeMenuOpen = false
                        })
                    }
                }

                // Operator dropdown
                var opMenuOpen by remember { mutableStateOf(false) }
                Box {
                    TextButton(onClick = { opMenuOpen = true }) {
                        Text(vm.searchRatingOp)
                        Icon(Icons.Default.ArrowDropDown, null, modifier = Modifier.size(18.dp))
                    }
                    DropdownMenu(expanded = opMenuOpen, onDismissRequest = { opMenuOpen = false }) {
                        for (op in listOf(">=", "<=", ">", "<", "=")) {
                            DropdownMenuItem(text = { Text(op) }, onClick = {
                                vm.setRatingFilter(vm.searchRatingType, op, vm.searchRatingValue ?: 5)
                                opMenuOpen = false
                            })
                        }
                    }
                }

                // Value slider
                val sliderValue = (vm.searchRatingValue ?: 5).toFloat()
                Text("${sliderValue.toInt()}", style = MaterialTheme.typography.bodyMedium, modifier = Modifier.width(24.dp))
                Slider(
                    value = sliderValue,
                    onValueChange = {
                        vm.setRatingFilter(vm.searchRatingType, vm.searchRatingOp, it.toInt())
                    },
                    valueRange = 1f..10f,
                    steps = 8,
                    modifier = Modifier.weight(1f)
                )
            }
        }
    }
}

// ==================== Queue ====================

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun QueueScreen(vm: MainViewModel, onSwitchToLibrary: () -> Unit = {}) {
    if (vm.queue.isEmpty()) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Icon(
                    Icons.AutoMirrored.Filled.QueueMusic,
                    contentDescription = null,
                    modifier = Modifier.size(48.dp),
                    tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
                )
                Spacer(Modifier.height(12.dp))
                Text(
                    "Queue is empty",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    } else {
        var dragFromPos by remember { mutableStateOf(-1) }
        var dragOffsetY by remember { mutableFloatStateOf(0f) }
        var itemHeight by remember { mutableFloatStateOf(0f) }
        val queueListState = rememberLazyListState()

        // Jump to the currently playing track when opening the queue
        LaunchedEffect(Unit) {
            val currentIdx = vm.queue.indexOfFirst { it.current }
            if (currentIdx >= 0) {
                // +1 for the "N tracks" header item
                queueListState.scrollToItem((currentIdx + 1 - 2).coerceAtLeast(0))
            }
        }

        LazyColumn(
            state = queueListState,
            modifier = Modifier.fillMaxSize(),
            // Disable list scrolling while dragging to prevent conflicts
            userScrollEnabled = dragFromPos < 0
        ) {
            item {
                Row(
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        "${vm.queue.size} tracks",
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    // Legend for the priority dots, shown only when relevant
                    if (vm.queue.any { it.priority > 0 }) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(4.dp)
                        ) {
                            Text("priority:", style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                            listOf(Color(0xFFFF6600) to "high", Color(0xFFFF9933) to "mid", Color(0xFFFFCC66) to "low").forEach { (c, l) ->
                                Text("●", color = c, style = MaterialTheme.typography.labelSmall)
                                Text(l, style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                }
            }
            items(
                vm.queue,
                // Stable identity across reorders keeps row state and
                // animations attached to the right track.
                key = { item -> if (item.queueId >= 0) item.queueId else "${item.songId}#${item.position}" }
            ) { item ->
                val isCurrent = item.current
                val isDragging = dragFromPos == item.position
                val density = androidx.compose.ui.platform.LocalDensity.current

                Surface(
                    color = if (isDragging) MaterialTheme.colorScheme.surfaceContainerHigh
                           else if (isCurrent) MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.45f)
                           else MaterialTheme.colorScheme.surface,
                    shadowElevation = if (isDragging) 8.dp else 0.dp,
                    modifier = Modifier
                        .then(if (isDragging) Modifier.zIndex(1f).offset(y = with(density) { dragOffsetY.toDp() }) else Modifier)
                ) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .combinedClickable(
                                onClick = { vm.queuePlay(item.position) },
                                onLongClick = { vm.showAction(MainViewModel.ActionTarget.QueueItemTarget(item)) }
                            )
                            .onGloballyPositioned { if (itemHeight == 0f) itemHeight = it.size.height.toFloat() }
                            .padding(start = 4.dp, end = 12.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        // Drag handle
                        Icon(
                            Icons.Default.DragHandle,
                            "Reorder",
                            tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
                            modifier = Modifier
                                .size(40.dp)
                                .padding(8.dp)
                                .pointerInput(Unit) {
                                    awaitPointerEventScope {
                                        while (true) {
                                            val down = awaitFirstDown(requireUnconsumed = false)
                                            dragFromPos = item.position
                                            dragOffsetY = 0f
                                            down.consume()

                                            while (true) {
                                                val event = awaitPointerEvent()
                                                val change = event.changes.firstOrNull() ?: break
                                                if (change.changedToUpIgnoreConsumed()) {
                                                    dragFromPos = -1
                                                    dragOffsetY = 0f
                                                    change.consume()
                                                    break
                                                }
                                                val dy = change.position.y - change.previousPosition.y
                                                dragOffsetY += dy
                                                change.consume()
                                                if (itemHeight > 0f) {
                                                    val steps = (dragOffsetY / itemHeight).toInt()
                                                    if (steps != 0) {
                                                        val newPos = (dragFromPos + steps).coerceIn(0, vm.queue.size - 1)
                                                        if (newPos != dragFromPos) {
                                                            // Optimistic: reorders the local list instantly
                                                            vm.queueMoveOptimistic(dragFromPos, newPos)
                                                            dragFromPos = newPos
                                                            dragOffsetY -= steps * itemHeight
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                        )

                        // Position / playing indicator
                        Box(modifier = Modifier.width(32.dp), contentAlignment = Alignment.Center) {
                            if (isCurrent) {
                                Icon(
                                    Icons.Default.PlayArrow,
                                    "Playing",
                                    tint = MaterialTheme.colorScheme.primary,
                                    modifier = Modifier.size(24.dp)
                                )
                            } else {
                                Text(
                                    "${item.position + 1}",
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    textAlign = TextAlign.Center
                                )
                            }
                        }

                        // Priority indicator
                        if (item.priority > 0) {
                            val prioColor = when {
                                item.priority >= 30 -> Color(0xFFFF6600)
                                item.priority >= 20 -> Color(0xFFFF9933)
                                else -> Color(0xFFFFCC66)
                            }
                            Text(
                                "\u25cf",
                                color = prioColor,
                                style = MaterialTheme.typography.bodySmall,
                                modifier = Modifier.padding(end = 4.dp)
                            )
                        }

                        // Track info
                        Column(
                            modifier = Modifier
                                .weight(1f)
                                .padding(horizontal = 8.dp, vertical = 12.dp)
                        ) {
                            Text(
                                item.title.ifBlank { "Unknown" },
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                style = MaterialTheme.typography.bodyLarge,
                                fontWeight = if (isCurrent) FontWeight.Bold else FontWeight.Normal,
                                color = if (isCurrent) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface
                            )
                            Text(
                                "${item.artist} \u2014 ${item.album}",
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                style = MaterialTheme.typography.bodyMedium,
                                color = if (isCurrent) MaterialTheme.colorScheme.primary.copy(alpha = 0.7f) else MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }

                        if (item.rating > 0) {
                            Row(modifier = Modifier.padding(end = 4.dp)) {
                                MiniStars(item.rating, 16.dp)
                            }
                        }

                        Text(
                            fmtTime(item.duration),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        }
    }

    // Queue item action sheet (triggered by long press)
    if (vm.showActionMenu && vm.actionTarget is MainViewModel.ActionTarget.QueueItemTarget) {
        val target = vm.actionTarget as MainViewModel.ActionTarget.QueueItemTarget
        val item = target.item
        val sheetState = rememberModalBottomSheetState()

        ModalBottomSheet(
            onDismissRequest = { vm.dismissAction() },
            sheetState = sheetState,
            containerColor = MaterialTheme.colorScheme.surface
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(bottom = 32.dp)
            ) {
                Text(
                    item.title,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(horizontal = 24.dp, vertical = 12.dp)
                )
                Text(
                    "${item.artist} \u2014 ${item.album}",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(horizontal = 24.dp)
                )
                Spacer(Modifier.height(8.dp))
                HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = 0.3f))

                ListItem(
                    headlineContent = { Text("Go to artist") },
                    leadingContent = { Icon(Icons.Default.Person, null) },
                    modifier = Modifier.clickable {
                        vm.goToArtistFromQueue(item)
                        vm.dismissAction()
                        onSwitchToLibrary()
                    }
                )
                if (item.albumId.isNotBlank()) {
                    ListItem(
                        headlineContent = { Text("Go to album") },
                        leadingContent = { Icon(Icons.Default.Album, null) },
                        modifier = Modifier.clickable {
                            vm.loadTracks(Album(item.albumId, item.artist, item.album, ""))
                            vm.dismissAction()
                            onSwitchToLibrary()
                        }
                    )
                }
                if (item.uri.isNotBlank()) {
                    ListItem(
                        headlineContent = { Text("Add to playlist") },
                        leadingContent = { Icon(Icons.AutoMirrored.Filled.PlaylistAdd, null) },
                        modifier = Modifier.clickable {
                            vm.dismissAction()
                            vm.showAddToPlaylist(item.uri)
                        }
                    )
                }
                if (item.songId.isNotBlank()) {
                    var trackRating by remember { mutableIntStateOf(item.rating) }
                    ListItem(
                        headlineContent = { Text("Rate track") },
                        leadingContent = { Icon(Icons.Default.Star, null) },
                        supportingContent = {
                            Row {
                                StarRating(rating = trackRating, boxSize = 36.dp, iconSize = 24.dp) {
                                    trackRating = it
                                    vm.rateQueueTrack(item.songId, it)
                                }
                            }
                        }
                    )
                }
                // Priority: prioritized tracks play first when random mode is on
                ListItem(
                    headlineContent = { Text("Priority") },
                    leadingContent = { Icon(Icons.Default.Equalizer, null) },
                    supportingContent = {
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            listOf(0 to "Off", 10 to "Low", 20 to "Mid", 30 to "High").forEach { (value, label) ->
                                FilterChip(
                                    selected = item.priority == value || (value == 0 && item.priority == 0),
                                    onClick = {
                                        vm.setQueuePriority(item.position, value)
                                        vm.dismissAction()
                                    },
                                    label = { Text(label) }
                                )
                            }
                        }
                    }
                )
                ListItem(
                    headlineContent = { Text("Remove from queue") },
                    leadingContent = { Icon(Icons.Default.Delete, null, tint = MaterialTheme.colorScheme.error) },
                    modifier = Modifier.clickable {
                        vm.queueRemove(item.position)
                        vm.dismissAction()
                    }
                )
            }
        }
    }
}

// ==================== Playlists ====================

@Composable
fun PlaylistsScreen(vm: MainViewModel) {
    BackHandler(enabled = vm.playlistView) {
        vm.playlistBack()
    }

    AnimatedContent(
        targetState = vm.playlistView,
        transitionSpec = {
            if (targetState) {
                (slideInHorizontally { it } + fadeIn()) togetherWith (slideOutHorizontally { -it } + fadeOut())
            } else {
                (slideInHorizontally { -it } + fadeIn()) togetherWith (slideOutHorizontally { it } + fadeOut())
            }
        },
        label = "playlists"
    ) { showTracks ->
        if (showTracks) {
            PlaylistTrackList(vm)
        } else {
            PlaylistList(vm)
        }
    }
}

@Composable
fun PlaylistList(vm: MainViewModel) {
    if (vm.playlists.isEmpty()) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Icon(
                    Icons.AutoMirrored.Filled.PlaylistPlay,
                    contentDescription = null,
                    modifier = Modifier.size(48.dp),
                    tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
                )
                Spacer(Modifier.height(12.dp))
                Text(
                    "No playlists",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    } else {
        LazyColumn(Modifier.fillMaxSize()) {
            item {
                Text(
                    "${vm.playlists.size} playlists",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
                )
            }
            itemsIndexed(vm.playlists) { _, playlist ->
                ListItem(
                    headlineContent = {
                        Text(playlist.name, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    },
                    supportingContent = {
                        val parts = mutableListOf<String>()
                        parts.add("${playlist.songCount} tracks")
                        if (playlist.duration > 0) {
                            val mins = playlist.duration / 60
                            if (mins >= 60) {
                                parts.add("${mins / 60}h ${mins % 60}m")
                            } else {
                                parts.add("${mins}m")
                            }
                        }
                        Text(parts.joinToString(" \u2022 "))
                    },
                    leadingContent = {
                        val coverUrl = if (playlist.coverArt.isNotBlank())
                            MelodyApp.instance.mpd.coverUrl(playlist.coverArt, 150)
                        else null
                        Box(
                            modifier = Modifier
                                .size(48.dp)
                                .clip(RoundedCornerShape(6.dp))
                                .background(MaterialTheme.colorScheme.surfaceVariant),
                            contentAlignment = Alignment.Center
                        ) {
                            if (coverUrl != null) {
                                AsyncImage(
                                    model = ImageRequest.Builder(MelodyApp.instance)
                                        .data(coverUrl)
                                        .crossfade(true)
                                        .build(),
                                    contentDescription = "Playlist art",
                                    modifier = Modifier.fillMaxSize(),
                                    contentScale = androidx.compose.ui.layout.ContentScale.Crop
                                )
                            } else {
                                Icon(
                                    Icons.AutoMirrored.Filled.PlaylistPlay, null,
                                    modifier = Modifier.size(24.dp),
                                    tint = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                        }
                    },
                    modifier = Modifier.clickable { vm.loadPlaylistTracks(playlist) },
                    trailingContent = {
                        IconButton(onClick = { vm.showAction(MainViewModel.ActionTarget.PlaylistTarget(playlist)) }) {
                            Icon(Icons.Default.MoreVert, "Actions")
                        }
                    }
                )
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun PlaylistTrackList(vm: MainViewModel) {
    LazyColumn(Modifier.fillMaxSize()) {
        item {
            Text(
                "${vm.playlistTracks.size} tracks",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
            )
        }
        itemsIndexed(vm.playlistTracks) { idx, track ->
            ListItem(
                leadingContent = {
                    Text(
                        "${idx + 1}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.width(28.dp),
                        textAlign = TextAlign.End
                    )
                },
                headlineContent = {
                    Text(track.title, maxLines = 1, overflow = TextOverflow.Ellipsis)
                },
                supportingContent = {
                    Text(
                        "${track.artist} \u2014 ${track.album}",
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                },
                trailingContent = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (track.duration > 0) {
                            Text(
                                fmtTime(track.duration),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                        IconButton(onClick = { vm.showAction(MainViewModel.ActionTarget.TrackTarget(track)) }) {
                            Icon(Icons.Default.MoreVert, "Actions")
                        }
                    }
                },
                // Tap = play the playlist from this track; menu via \u22ee or long-press
                modifier = Modifier.combinedClickable(
                    onClick = { vm.playTrackInContext(vm.playlistTracks, idx) },
                    onLongClick = { vm.showAction(MainViewModel.ActionTarget.TrackTarget(track)) }
                )
            )
        }
    }
}

// ==================== Devices Sheet ====================

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DevicesSheet(vm: MainViewModel, onDismiss: () -> Unit) {
    val sheetState = rememberModalBottomSheetState()
    LaunchedEffect(Unit) {
        vm.loadDevices()
    }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        containerColor = MaterialTheme.colorScheme.surface
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 32.dp)
        ) {
            Text(
                "Devices",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.padding(horizontal = 24.dp, vertical = 12.dp)
            )
            HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = 0.3f))

            if (vm.devices.isEmpty()) {
                Box(
                    Modifier
                        .fillMaxWidth()
                        .padding(48.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        "No devices found",
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            } else {
                vm.devices.forEach { dev ->
                    ListItem(
                        headlineContent = {
                            Text(
                                dev.name,
                                fontWeight = if (dev.active) FontWeight.SemiBold else FontWeight.Normal,
                                color = if (dev.active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface
                            )
                        },
                        supportingContent = {
                            val parts = mutableListOf<String>()
                            parts.add(when (dev.type) {
                                "local" -> "Server"
                                "browser" -> "Mobile"
                                "agent" -> "Agent"
                                else -> dev.type
                            })
                            if (dev.format.isNotBlank()) {
                                var q = dev.format
                                if (dev.maxBitrate > 0) q += " ${dev.maxBitrate}k"
                                parts.add(q)
                            }
                            Text(parts.joinToString(" \u2022 "))
                        },
                        leadingContent = {
                            Box(
                                Modifier
                                    .size(10.dp)
                                    .clip(CircleShape)
                                    .background(
                                        if (dev.online) Color(0xFF22C55E) else MaterialTheme.colorScheme.outline
                                    )
                            )
                        },
                        trailingContent = {
                            if (dev.active) {
                                Icon(
                                    Icons.AutoMirrored.Filled.VolumeUp,
                                    "Active",
                                    tint = MaterialTheme.colorScheme.primary
                                )
                            }
                        },
                        modifier = Modifier.clickable {
                            vm.setActiveDevice(dev.id)
                            onDismiss()
                        }
                    )
                }
            }
        }
    }
}

// ==================== Playlist Picker ====================

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PlaylistPickerSheet(vm: MainViewModel) {
    val sheetState = rememberModalBottomSheetState()
    var showNewPlaylist by remember { mutableStateOf(false) }
    var newPlaylistName by remember { mutableStateOf("") }

    ModalBottomSheet(
        onDismissRequest = { vm.dismissPlaylistPicker() },
        sheetState = sheetState,
        containerColor = MaterialTheme.colorScheme.surface
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 32.dp)
        ) {
            Text(
                "Add to playlist",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.padding(horizontal = 24.dp, vertical = 12.dp)
            )
            HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = 0.3f))

            if (showNewPlaylist) {
                Row(
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    OutlinedTextField(
                        value = newPlaylistName,
                        onValueChange = { newPlaylistName = it },
                        placeholder = { Text("Playlist name") },
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                        shape = RoundedCornerShape(12.dp),
                        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                        keyboardActions = KeyboardActions(onDone = {
                            if (newPlaylistName.isNotBlank()) {
                                vm.addToPlaylist(newPlaylistName.trim())
                            }
                        })
                    )
                    Spacer(Modifier.width(8.dp))
                    TextButton(
                        onClick = {
                            if (newPlaylistName.isNotBlank()) {
                                vm.addToPlaylist(newPlaylistName.trim())
                            }
                        }
                    ) {
                        Text("Add")
                    }
                }
            } else {
                ListItem(
                    headlineContent = { Text("New playlist") },
                    leadingContent = {
                        Icon(Icons.Default.Add, null, tint = MaterialTheme.colorScheme.primary)
                    },
                    modifier = Modifier.clickable { showNewPlaylist = true }
                )
            }

            LazyColumn {
                itemsIndexed(vm.playlists) { _, playlist ->
                    ListItem(
                        headlineContent = {
                            Text(playlist.name, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        },
                        supportingContent = {
                            Text("${playlist.songCount} tracks")
                        },
                        leadingContent = {
                            Icon(
                                Icons.AutoMirrored.Filled.PlaylistPlay, null,
                                tint = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        },
                        modifier = Modifier.clickable { vm.addToPlaylist(playlist.name) }
                    )
                }
            }
        }
    }
}

// ==================== Settings ====================

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun SettingsScreen(vm: MainViewModel, onDismiss: () -> Unit) {
    val prefs = MelodyApp.instance.getSharedPreferences(
        "melody", android.content.Context.MODE_PRIVATE
    )
    var server by remember { mutableStateOf(prefs.getString("server", "") ?: "") }
    var externalServer by remember { mutableStateOf(prefs.getString("external_server", "") ?: "") }
    var homeWifiSsid by remember { mutableStateOf(prefs.getString("home_wifi_ssid", "") ?: "") }
    var deviceName by remember {
        mutableStateOf(
            prefs.getString("device_name", null)
                ?: "android-${android.os.Build.MODEL}".replace(" ", "-").lowercase()
        )
    }
    var format by remember { mutableStateOf(prefs.getString("audio_format", "") ?: "") }
    var bitrate by remember { mutableIntStateOf(prefs.getInt("audio_bitrate", 0)) }
    var replaygain by remember { mutableStateOf(prefs.getString("replaygain", "off") ?: "off") }

    // Snapshot of agent-relevant settings at screen entry. The agent is
    // re-registered ONCE when leaving settings — reconnecting on every chip
    // tap audibly interrupts playback while the user experiments.
    val initialAgentSettings = remember {
        Triple(
            prefs.getString("device_name", "") ?: "",
            prefs.getString("audio_format", "") ?: "",
            prefs.getInt("audio_bitrate", 0)
        )
    }

    fun saveAll() {
        prefs.edit()
            .putString("server", server)
            .putString("external_server", externalServer)
            .putString("home_wifi_ssid", homeWifiSsid)
            .putString("device_name", deviceName)
            .putString("audio_format", format)
            .putInt("audio_bitrate", bitrate)
            .putString("replaygain", replaygain)
            .apply()
    }

    fun closeSettings() {
        saveAll()
        MelodyApp.instance.applyServerForCurrentNetwork()
        val (oldName, oldFormat, oldBitrate) = initialAgentSettings
        if (deviceName != oldName || format != oldFormat || bitrate != oldBitrate) {
            PlaybackService.instance?.reconnect()
        }
        onDismiss()
    }

    BackHandler { closeSettings() }

    // Dialog state
    var editingField by remember { mutableStateOf<String?>(null) }
    var editValue by remember { mutableStateOf("") }

    // Edit dialog
    if (editingField != null) {
        val fieldLabel = when (editingField) {
            "server" -> "Local server address"
            "external_server" -> "External server address"
            "home_wifi_ssid" -> "Home WiFi SSID"
            "device_name" -> "Device name"
            else -> ""
        }
        AlertDialog(
            onDismissRequest = { editingField = null },
            title = { Text(fieldLabel) },
            text = {
                Column {
                    OutlinedTextField(
                        value = editValue,
                        onValueChange = { editValue = it },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                        placeholder = {
                            Text(when (editingField) {
                                "server" -> "192.168.1.10:6701"
                                "external_server" -> "https://music.example.com"
                                "home_wifi_ssid" -> "MyHomeNetwork"
                                else -> ""
                            })
                        }
                    )
                    // Show current WiFi hint for SSID field
                    if (editingField == "home_wifi_ssid") {
                        val currentSsid = remember { MelodyApp.instance.getCurrentSSID() }
                        if (currentSsid != null) {
                            Spacer(Modifier.height(8.dp))
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Text(
                                    "Current: $currentSsid",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    modifier = Modifier.weight(1f)
                                )
                                TextButton(onClick = { editValue = currentSsid }) {
                                    Text("Use current")
                                }
                            }
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    when (editingField) {
                        "server" -> server = editValue
                        "external_server" -> externalServer = editValue
                        "home_wifi_ssid" -> homeWifiSsid = editValue
                        "device_name" -> deviceName = editValue
                    }
                    editingField = null
                    saveAll()
                }) {
                    Text("Save")
                }
            },
            dismissButton = {
                TextButton(onClick = { editingField = null }) {
                    Text("Cancel")
                }
            }
        )
    }

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = MaterialTheme.colorScheme.background
    ) {
        Column(Modifier.fillMaxSize()) {
            TopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    IconButton(onClick = { closeSettings() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface
                )
            )

            LazyColumn(Modifier.fillMaxSize()) {
                // --- Connection section ---
                item {
                    SettingsSectionHeader("Connection")
                }
                item {
                    SettingsTextItem(
                        title = "Local server address",
                        value = server.ifBlank { "Not set" },
                        onClick = {
                            editValue = server
                            editingField = "server"
                        }
                    )
                }
                item {
                    SettingsTextItem(
                        title = "External server address",
                        value = externalServer.ifBlank { "Not set" },
                        subtitle = "Used when not on home WiFi",
                        onClick = {
                            editValue = externalServer
                            editingField = "external_server"
                        }
                    )
                }
                item {
                    val currentSsid = remember { MelodyApp.instance.getCurrentSSID() }
                    SettingsTextItem(
                        title = "Home WiFi SSID",
                        value = homeWifiSsid.ifBlank { "Not set" },
                        subtitle = if (currentSsid != null) "Current: $currentSsid" else "Not on WiFi",
                        onClick = {
                            editValue = homeWifiSsid
                            editingField = "home_wifi_ssid"
                        }
                    )
                }

                // --- Device section ---
                item {
                    SettingsSectionHeader("Device")
                }
                item {
                    SettingsTextItem(
                        title = "Device name",
                        value = deviceName,
                        onClick = {
                            editValue = deviceName
                            editingField = "device_name"
                        }
                    )
                }

                // --- Audio section ---
                item {
                    SettingsSectionHeader("Audio")
                }
                item {
                    SettingsChipRow(
                        title = "Format",
                        options = listOf("" to "Original", "opus" to "Opus", "mp3" to "MP3", "aac" to "AAC", "flac" to "FLAC"),
                        selected = format,
                        onSelect = { format = it; saveAll() }
                    )
                }
                item {
                    SettingsChipRow(
                        title = "Bitrate",
                        options = listOf(0 to "Max", 64 to "64k", 128 to "128k", 192 to "192k", 256 to "256k", 320 to "320k"),
                        selected = bitrate,
                        onSelect = { bitrate = it; saveAll() }
                    )
                }
                item {
                    var directOnWifi by remember {
                        mutableStateOf(prefs.getBoolean("direct_on_wifi", true))
                    }
                    ListItem(
                        headlineContent = { Text("Original on home WiFi") },
                        supportingContent = { Text("Stream untranscoded on home WiFi; use the format above on mobile") },
                        trailingContent = {
                            Switch(
                                checked = directOnWifi,
                                onCheckedChange = {
                                    directOnWifi = it
                                    prefs.edit().putBoolean("direct_on_wifi", it).apply()
                                    PlaybackService.instance?.reconnect()
                                }
                            )
                        }
                    )
                }
                item {
                    // Reflect the server's live mode (it may have been changed
                    // from another client), falling back to the local pref.
                    SettingsChipRow(
                        title = "ReplayGain",
                        options = listOf("off" to "Off", "track" to "Track", "album" to "Album"),
                        selected = vm.status?.replayGainMode ?: replaygain,
                        onSelect = {
                            replaygain = it; saveAll()
                            vm.setReplayGain(it)
                        }
                    )
                }

                // --- Playback section ---
                item {
                    SettingsSectionHeader("Playback")
                }
                item {
                    var resumeOnConnect by remember {
                        mutableStateOf(prefs.getBoolean("resume_on_connect", false))
                    }
                    ListItem(
                        headlineContent = { Text("Resume on connect") },
                        supportingContent = { Text("Auto-resume playback when connecting to server") },
                        trailingContent = {
                            Switch(
                                checked = resumeOnConnect,
                                onCheckedChange = {
                                    resumeOnConnect = it
                                    prefs.edit().putBoolean("resume_on_connect", it).apply()
                                }
                            )
                        }
                    )
                }

                // --- Downloads section ---
                item {
                    SettingsSectionHeader("Downloads")
                }
                item {
                    var wifiOnly by remember {
                        mutableStateOf(prefs.getBoolean("download_wifi_only", true))
                    }
                    ListItem(
                        headlineContent = { Text("Wi-Fi only") },
                        supportingContent = { Text("Pause offline downloads on metered networks") },
                        trailingContent = {
                            Switch(
                                checked = wifiOnly,
                                onCheckedChange = {
                                    wifiOnly = it
                                    prefs.edit().putBoolean("download_wifi_only", it).apply()
                                }
                            )
                        }
                    )
                }

                // --- Library section ---
                item {
                    SettingsSectionHeader("Library")
                }
                item {
                    ListItem(
                        headlineContent = { Text("Update library") },
                        supportingContent = { Text("Rescan the server's music directory") },
                        leadingContent = { Icon(Icons.Default.Refresh, null) },
                        modifier = Modifier.clickable { vm.updateLibrary() }
                    )
                }

                // --- Appearance ---
                item {
                    SettingsSectionHeader("Appearance")
                }
                item {
                    SettingsChipRow(
                        title = "Theme",
                        options = listOf("system" to "System", "dark" to "Dark", "light" to "Light"),
                        selected = ThemePrefs.mode,
                        onSelect = {
                            ThemePrefs.mode = it
                            ThemePrefs.save(MelodyApp.instance)
                        }
                    )
                }
                item {
                    ListItem(
                        headlineContent = { Text("Dynamic colors") },
                        supportingContent = { Text("Use Material You colors from your wallpaper (Android 12+)") },
                        trailingContent = {
                            Switch(
                                checked = ThemePrefs.dynamic,
                                onCheckedChange = {
                                    ThemePrefs.dynamic = it
                                    ThemePrefs.save(MelodyApp.instance)
                                }
                            )
                        }
                    )
                }

                // --- About ---
                item {
                    SettingsSectionHeader("About")
                }
                item {
                    val context = androidx.compose.ui.platform.LocalContext.current
                    val versionName = remember {
                        try {
                            context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "?"
                        } catch (_: Exception) { "?" }
                    }
                    ListItem(
                        headlineContent = { Text("Version") },
                        supportingContent = { Text(versionName) }
                    )
                }

                // Bottom spacing
                item { Spacer(Modifier.height(32.dp)) }
            }
        }
    }
}

@Composable
fun SettingsSectionHeader(title: String) {
    Text(
        title,
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.primary,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 24.dp, bottom = 8.dp)
    )
}

@Composable
fun SettingsTextItem(
    title: String,
    value: String,
    subtitle: String? = null,
    onClick: () -> Unit
) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = {
            Column {
                Text(
                    value,
                    color = if (value == "Not set") MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
                    else MaterialTheme.colorScheme.onSurfaceVariant
                )
                if (subtitle != null) {
                    Text(
                        subtitle,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                    )
                }
            }
        },
        modifier = Modifier.clickable(onClick = onClick)
    )
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun <T> SettingsChipRow(
    title: String,
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit
) {
    Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
        Text(
            title,
            style = MaterialTheme.typography.bodyLarge
        )
        Spacer(Modifier.height(8.dp))
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            options.forEach { (value, label) ->
                FilterChip(
                    selected = selected == value,
                    onClick = { onSelect(value) },
                    label = { Text(label) }
                )
            }
        }
    }
}

@Composable
fun MiniStars(rating: Int, starSize: Dp) {
    val full = rating / 2
    val half = rating % 2
    for (i in 1..full) {
        Icon(Icons.Default.Star, contentDescription = null, tint = Color(0xFFE6B422), modifier = Modifier.size(starSize))
    }
    if (half > 0) {
        Icon(Icons.AutoMirrored.Filled.StarHalf, contentDescription = null, tint = Color(0xFFE6B422), modifier = Modifier.size(starSize))
    }
}

// ==================== Action Sheet ====================

/**
 * Shared star rating widget (0-10 scale, 5 stars, half steps).
 *
 * Tap-to-cycle interaction: tapping a star sets it as a full star; tapping
 * the same star again refines to a half star; a third tap clears the rating.
 * Discoverable through natural use — unlike invisible left/right tap zones —
 * and gives TalkBack one labeled target per star.
 *
 * [rating] is what's displayed (may be a computed album average, dimmed via
 * [computed]); [cycleFrom] is the user's own rating that cycling starts from.
 */
@Composable
fun StarRating(
    rating: Int,
    boxSize: Dp,
    iconSize: Dp,
    computed: Boolean = false,
    cycleFrom: Int = rating,
    onRate: (Int) -> Unit
) {
    for (starPos in 1..5) {
        val fullValue = starPos * 2
        val halfValue = fullValue - 1
        val icon = when {
            rating >= fullValue -> Icons.Default.Star
            rating >= halfValue -> Icons.AutoMirrored.Filled.StarHalf
            else -> Icons.Default.StarOutline
        }
        val gold = Color(0xFFE6B422)
        val tint = when {
            rating >= halfValue && computed -> gold.copy(alpha = 0.5f)
            rating >= halfValue -> gold
            else -> MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)
        }
        Box(
            modifier = Modifier
                .size(boxSize)
                .clickable(onClickLabel = "Rate $starPos stars, tap again for half star, again to clear") {
                    onRate(
                        when (cycleFrom) {
                            fullValue -> halfValue
                            halfValue -> 0
                            else -> fullValue
                        }
                    )
                },
            contentAlignment = Alignment.Center
        ) {
            Icon(icon, contentDescription = null, tint = tint, modifier = Modifier.size(iconSize))
        }
    }
}

@Composable
fun RatingBar(rating: Int, onRate: (Int) -> Unit) {
    Row(
        horizontalArrangement = Arrangement.Center,
        modifier = Modifier.fillMaxWidth()
    ) {
        StarRating(rating = rating, boxSize = 44.dp, iconSize = 28.dp, onRate = onRate)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ActionSheet(vm: MainViewModel) {
    val target = vm.actionTarget ?: return
    val sheetState = rememberModalBottomSheetState()
    val label = when (target) {
        is MainViewModel.ActionTarget.ArtistTarget -> target.name
        is MainViewModel.ActionTarget.AlbumTarget -> target.album.album
        is MainViewModel.ActionTarget.TrackTarget -> target.track.title
        is MainViewModel.ActionTarget.SearchAlbumTarget -> target.album.album
        is MainViewModel.ActionTarget.SearchTrackTarget -> target.track.title
        is MainViewModel.ActionTarget.QueueItemTarget -> target.item.title
        is MainViewModel.ActionTarget.PlaylistTarget -> target.playlist.name
    }
    val canBrowse = target !is MainViewModel.ActionTarget.TrackTarget &&
            target !is MainViewModel.ActionTarget.SearchTrackTarget &&
            target !is MainViewModel.ActionTarget.PlaylistTarget

    ModalBottomSheet(
        onDismissRequest = { vm.dismissAction() },
        sheetState = sheetState,
        containerColor = MaterialTheme.colorScheme.surface
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 32.dp)
        ) {
            // Title
            Text(
                label,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(horizontal = 24.dp, vertical = 12.dp)
            )
            HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = 0.3f))

            // Actions
            ListItem(
                headlineContent = { Text("Add to queue") },
                leadingContent = { Icon(Icons.AutoMirrored.Filled.PlaylistAdd, null) },
                modifier = Modifier.clickable { vm.executeAction("add") }
            )
            ListItem(
                headlineContent = { Text("Insert after current") },
                leadingContent = { Icon(Icons.Default.Add, null) },
                modifier = Modifier.clickable { vm.executeAction("insert") }
            )
            ListItem(
                headlineContent = { Text("Replace queue") },
                leadingContent = { Icon(Icons.AutoMirrored.Filled.PlaylistPlay, null) },
                modifier = Modifier.clickable { vm.executeAction("replace") }
            )
            if (canBrowse) {
                ListItem(
                    headlineContent = { Text("Browse into") },
                    leadingContent = { Icon(Icons.Default.FolderOpen, null) },
                    modifier = Modifier.clickable { vm.browseIntoAction() }
                )
            }

            // Add to playlist option for tracks
            val uriForPlaylist = when (target) {
                is MainViewModel.ActionTarget.TrackTarget -> target.track.uri
                is MainViewModel.ActionTarget.SearchTrackTarget -> target.track.uri
                else -> null
            }
            if (uriForPlaylist != null && uriForPlaylist.isNotBlank()) {
                ListItem(
                    headlineContent = { Text("Add to playlist") },
                    leadingContent = { Icon(Icons.AutoMirrored.Filled.PlaylistAdd, null) },
                    modifier = Modifier.clickable {
                        vm.dismissAction()
                        vm.showAddToPlaylist(uriForPlaylist)
                    }
                )
            }

            // Download option for albums
            val albumForDownload = when (target) {
                is MainViewModel.ActionTarget.AlbumTarget -> target.album
                is MainViewModel.ActionTarget.SearchAlbumTarget -> target.album
                else -> null
            }
            // Rate option for albums
            val albumForRating = when (target) {
                is MainViewModel.ActionTarget.AlbumTarget -> target.album
                is MainViewModel.ActionTarget.SearchAlbumTarget -> target.album
                else -> null
            }
            if (albumForRating != null) {
                var albumRating by remember { mutableIntStateOf(0) }
                LaunchedEffect(albumForRating) {
                    try {
                        val r = MelodyApp.instance.mpd.getAlbumRating(albumForRating.albumArtist, albumForRating.album, albumForRating.date)
                        albumRating = r.rating
                    } catch (_: Exception) {}
                }
                ListItem(
                    headlineContent = { Text("Rate album") },
                    leadingContent = { Icon(Icons.Default.Star, null) },
                    supportingContent = {
                        Row {
                            StarRating(rating = albumRating, boxSize = 36.dp, iconSize = 24.dp) {
                                albumRating = it
                                vm.rateAlbumDirect(albumForRating, it)
                            }
                        }
                    }
                )
            }

            if (albumForDownload != null) {
                val isDownloaded = vm.isAlbumDownloaded(albumForDownload.id)
                if (isDownloaded) {
                    ListItem(
                        headlineContent = { Text("Remove download") },
                        leadingContent = { Icon(Icons.Default.Delete, null) },
                        modifier = Modifier.clickable {
                            vm.removeOfflineAlbum(albumForDownload.id)
                            vm.dismissAction()
                        }
                    )
                } else {
                    ListItem(
                        headlineContent = { Text("Download for offline") },
                        leadingContent = { Icon(Icons.Default.Download, null) },
                        modifier = Modifier.clickable {
                            vm.downloadAlbum(albumForDownload)
                            vm.dismissAction()
                        }
                    )
                }
            }

            // Delete option for playlists (with confirmation)
            if (target is MainViewModel.ActionTarget.PlaylistTarget) {
                var confirmDelete by remember { mutableStateOf(false) }
                ListItem(
                    headlineContent = { Text("Delete playlist") },
                    leadingContent = { Icon(Icons.Default.Delete, null, tint = MaterialTheme.colorScheme.error) },
                    modifier = Modifier.clickable { confirmDelete = true }
                )
                if (confirmDelete) {
                    AlertDialog(
                        onDismissRequest = { confirmDelete = false },
                        title = { Text("Delete playlist?") },
                        text = { Text("\"${target.playlist.name}\" will be permanently deleted.") },
                        confirmButton = {
                            TextButton(onClick = {
                                confirmDelete = false
                                vm.deletePlaylist(target.playlist)
                                vm.dismissAction()
                            }) { Text("Delete", color = MaterialTheme.colorScheme.error) }
                        },
                        dismissButton = {
                            TextButton(onClick = { confirmDelete = false }) { Text("Cancel") }
                        }
                    )
                }
            }
        }
    }
}

// ==================== Helpers ====================

fun fmtTime(seconds: Double): String {
    if (seconds < 0 || seconds.isNaN()) return "0:00"
    val m = (seconds / 60).toInt()
    val s = (seconds % 60).toInt()
    return "$m:${s.toString().padStart(2, '0')}"
}
