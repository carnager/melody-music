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

@Composable
fun MelodyTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
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
        ),
        content = content
    )
}

// ==================== Setup ====================

@Composable
fun SetupScreen(onConnected: () -> Unit) {
    var server by remember { mutableStateOf("") }
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
            Spacer(Modifier.height(20.dp))
            FilledIconButton(
                onClick = {
                    if (server.isNotBlank()) {
                        MelodyApp.instance.updateServer(server.trim())
                        onConnected()
                    }
                },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(48.dp),
                shape = RoundedCornerShape(12.dp)
            ) {
                Text("Connect", style = MaterialTheme.typography.labelLarge)
            }
        }
    }
}

// ==================== Main ====================

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(vm: MainViewModel) {
    var selectedTab by remember { mutableIntStateOf(0) }
    var showNowPlaying by remember { mutableStateOf(false) }
    var showSettings by remember { mutableStateOf(false) }
    var showDevices by remember { mutableStateOf(false) }

    // Derive top bar title from current state
    val topBarTitle = when (selectedTab) {
        0 -> when (vm.libView) {
            LibView.Artists -> "Library"
            LibView.Albums -> vm.curArtist
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
                                    Icon(Icons.Default.Shuffle, "Random album")
                                }
                            }
                            2 -> if (vm.queue.isNotEmpty()) {
                                IconButton(onClick = { vm.queueClear() }) {
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
                            onClick = { selectedTab = 3; vm.loadPlaylists() },
                            icon = { Icon(Icons.AutoMirrored.Filled.PlaylistPlay, contentDescription = null) },
                            label = { Text("Playlists") }
                        )
                    }
                }
            }
        ) { padding ->
            Box(Modifier.padding(padding)) {
                when (selectedTab) {
                    0 -> LibraryScreen(vm)
                    1 -> SearchScreen(vm)
                    2 -> QueueScreen(vm, onSwitchToLibrary = { selectedTab = 0 })
                    3 -> PlaylistsScreen(vm)
                }
            }
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

        // "Play on phone?" prompt
        if (vm.showPhonePrompt) {
            AlertDialog(
                onDismissRequest = { vm.phonePromptDismiss() },
                title = { Text("Play on phone?") },
                text = { Text("You're on mobile data. Switch playback to this device?") },
                confirmButton = {
                    TextButton(onClick = { vm.phonePromptConfirm() }) { Text("Phone") }
                },
                dismissButton = {
                    TextButton(onClick = { vm.phonePromptDismiss() }) { Text("Keep current") }
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
            SettingsScreen(onDismiss = { showSettings = false })
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
            val displayFraction = if (dragging) dragValue else if (dur > 0) (pos / dur).toFloat().coerceIn(0f, 1f) else 0f

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
                    fmtTime(if (dragging) dragValue.toDouble() * dur else pos),
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

            Spacer(Modifier.weight(1f))
        }
    }
}

@Composable
fun ModeIconButton(icon: androidx.compose.ui.graphics.vector.ImageVector, description: String, active: Boolean, onClick: () -> Unit) {
    val tint = if (active) Color.White else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)
    IconButton(onClick = onClick, modifier = Modifier.size(44.dp)) {
        Icon(icon, contentDescription = description, modifier = Modifier.size(24.dp), tint = tint)
    }
}

@Composable
fun PacManButton(active: Boolean, onClick: () -> Unit) {
    val color = if (active) Color.White else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)
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
            Text("Loading...", color = MaterialTheme.colorScheme.onSurfaceVariant)
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
                    Row {
                        IconButton(onClick = { vm.toggleLibSortLatest() }) {
                            Icon(
                                Icons.Default.Schedule,
                                contentDescription = "Sort by latest",
                                tint = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                        IconButton(onClick = { vm.toggleCachedOnly() }) {
                            Icon(
                                Icons.Default.FilterList,
                                contentDescription = if (vm.showCachedOnly) "Show all" else "Show offline only",
                                tint = if (vm.showCachedOnly) MaterialTheme.colorScheme.primary
                                       else MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
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
            items(vm.albums, key = { it.id.ifBlank { "${it.albumArtist}\u0000${it.album}" } }) { album ->
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
                        val displayRating = if (vm.albumRating > 0) vm.albumRating
                            else vm.albumComputedRating.let { if (it > 0.0) kotlin.math.round(it).toInt().coerceIn(1, 10) else 0 }
                        val isComputed = vm.albumRating == 0 && displayRating > 0
                        Spacer(Modifier.height(6.dp))
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            for (starPos in 1..5) {
                                val fullValue = starPos * 2
                                val halfValue = starPos * 2 - 1
                                val icon = when {
                                    displayRating >= fullValue -> Icons.Default.Star
                                    displayRating >= halfValue -> Icons.AutoMirrored.Filled.StarHalf
                                    else -> Icons.Default.StarOutline
                                }
                                val filled = displayRating >= halfValue
                                val tint = if (filled) {
                                    if (isComputed) Color(0xFFE6B422).copy(alpha = 0.5f)
                                    else Color(0xFFE6B422)
                                } else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)
                                Box(modifier = Modifier.size(22.dp)) {
                                    Icon(icon, contentDescription = null, tint = tint, modifier = Modifier.fillMaxSize())
                                    Row(Modifier.matchParentSize()) {
                                        Box(Modifier.weight(1f).fillMaxHeight().clickable {
                                            vm.rateAlbum(if (vm.albumRating == halfValue) 0 else halfValue)
                                        })
                                        Box(Modifier.weight(1f).fillMaxHeight().clickable {
                                            vm.rateAlbum(if (vm.albumRating == fullValue) 0 else fullValue)
                                        })
                                    }
                                }
                            }
                        }
                    }
                }
                HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = 0.3f))
            }
        }
        itemsIndexed(vm.tracks) { _, track ->
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
                supportingContent = {
                    Text(track.artist, maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodySmall)
                },
                modifier = Modifier.clickable { vm.showAction(MainViewModel.ActionTarget.TrackTarget(track)) }
            )
        }
    }
}

// ==================== Search ====================

@Composable
@OptIn(ExperimentalFoundationApi::class)
fun SearchScreen(vm: MainViewModel) {
    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            OutlinedTextField(
                value = vm.searchQuery,
                onValueChange = { vm.updateSearch(it) },
                placeholder = { Text("Search albums and tracks\u2026") },
                singleLine = true,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                shape = RoundedCornerShape(16.dp),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                keyboardActions = KeyboardActions(onSearch = {}),
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

        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            // Disable list scrolling while dragging to prevent conflicts
            userScrollEnabled = dragFromPos < 0
        ) {
            item {
                Text(
                    "${vm.queue.size} tracks",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
                )
            }
            itemsIndexed(vm.queue) { _, item ->
                val isCurrent = item.current
                val isDragging = dragFromPos == item.position
                val density = androidx.compose.ui.platform.LocalDensity.current

                Surface(
                    color = if (isDragging) MaterialTheme.colorScheme.surfaceContainerHigh
                           else if (isCurrent) Color(0xFF2A3F5F)
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
                                                            vm.queueMove(dragFromPos, newPos)
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
                                for (starPos in 1..5) {
                                    val halfValue = starPos * 2 - 1
                                    val fullValue = starPos * 2
                                    val icon = when {
                                        trackRating >= fullValue -> Icons.Default.Star
                                        trackRating >= halfValue -> Icons.AutoMirrored.Filled.StarHalf
                                        else -> Icons.Default.StarOutline
                                    }
                                    val filled = trackRating >= halfValue
                                    Box(modifier = Modifier.size(36.dp)) {
                                        Icon(
                                            icon,
                                            contentDescription = null,
                                            tint = if (filled) Color(0xFFE6B422)
                                                   else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
                                            modifier = Modifier.size(24.dp).align(Alignment.Center)
                                        )
                                        Row(Modifier.matchParentSize()) {
                                            Box(Modifier.weight(1f).fillMaxHeight().clickable {
                                                val v = halfValue; trackRating = if (trackRating == v) 0 else v
                                                vm.rateQueueTrack(item.songId, trackRating)
                                            })
                                            Box(Modifier.weight(1f).fillMaxHeight().clickable {
                                                val v = fullValue; trackRating = if (trackRating == v) 0 else v
                                                vm.rateQueueTrack(item.songId, trackRating)
                                            })
                                        }
                                    }
                                }
                            }
                        }
                    )
                }
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
                modifier = Modifier.clickable { vm.showAction(MainViewModel.ActionTarget.TrackTarget(track)) }
            )
        }
    }
}

// ==================== Devices Sheet ====================

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DevicesSheet(vm: MainViewModel, onDismiss: () -> Unit) {
    val sheetState = rememberModalBottomSheetState()

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
fun SettingsScreen(onDismiss: () -> Unit) {
    BackHandler { onDismiss() }

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
    var deviceSecret by remember { mutableStateOf(prefs.getString("device_secret", "") ?: "") }

    // Dialog state
    var editingField by remember { mutableStateOf<String?>(null) }
    var editValue by remember { mutableStateOf("") }

    fun saveAll() {
        // Check if agent-relevant settings changed before saving
        val oldName = prefs.getString("device_name", "") ?: ""
        val oldFormat = prefs.getString("audio_format", "") ?: ""
        val oldBitrate = prefs.getInt("audio_bitrate", 0)
        val agentChanged = deviceName != oldName || format != oldFormat || bitrate != oldBitrate

        prefs.edit()
            .putString("server", server)
            .putString("external_server", externalServer)
            .putString("home_wifi_ssid", homeWifiSsid)
            .putString("device_name", deviceName)
            .putString("audio_format", format)
            .putInt("audio_bitrate", bitrate)
            .putString("replaygain", replaygain)
            .putString("device_secret", deviceSecret)
            .apply()
        MelodyApp.instance.applyServerForCurrentNetwork()
        if (agentChanged) {
            PlaybackService.instance?.reconnect()
        }
    }

    // Edit dialog
    if (editingField != null) {
        val fieldLabel = when (editingField) {
            "server" -> "Local server address"
            "external_server" -> "External server address"
            "home_wifi_ssid" -> "Home WiFi SSID"
            "device_name" -> "Device name"
            "device_secret" -> "Device secret"
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
                        "device_secret" -> deviceSecret = editValue
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
                    IconButton(onClick = {
                        saveAll()
                        onDismiss()
                    }) {
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
                item {
                    SettingsTextItem(
                        title = "Device secret",
                        value = if (deviceSecret.isBlank()) "Not set" else "\u2022".repeat(deviceSecret.length.coerceAtMost(16)),
                        subtitle = "Shared secret for authenticated communication",
                        onClick = {
                            editValue = deviceSecret
                            editingField = "device_secret"
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
                    val rgScope = rememberCoroutineScope()
                    SettingsChipRow(
                        title = "ReplayGain",
                        options = listOf("off" to "Off", "track" to "Track", "album" to "Album"),
                        selected = replaygain,
                        onSelect = {
                            replaygain = it; saveAll()
                            rgScope.launch {
                                try { MelodyApp.instance.mpd.cmd("replay_gain_mode $it") }
                                catch (_: Exception) {}
                            }
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

@Composable
fun RatingBar(rating: Int, onRate: (Int) -> Unit) {
    // rating is 0-10: each star position covers 2 values (half star = odd, full = even)
    Row(
        horizontalArrangement = Arrangement.Center,
        modifier = Modifier.fillMaxWidth()
    ) {
        for (starPos in 1..5) {
            val halfValue = starPos * 2 - 1  // 1,3,5,7,9
            val fullValue = starPos * 2      // 2,4,6,8,10
            val icon = when {
                rating >= fullValue -> Icons.Default.Star
                rating >= halfValue -> Icons.AutoMirrored.Filled.StarHalf
                else -> Icons.Default.StarOutline
            }
            val tint = if (rating >= halfValue) Color(0xFFE6B422)
                       else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)
            Box(modifier = Modifier.size(44.dp)) {
                Icon(
                    icon,
                    contentDescription = "${starPos} stars",
                    tint = tint,
                    modifier = Modifier.size(28.dp).align(Alignment.Center)
                )
                // Left half tap = half star (odd), right half tap = full star (even)
                Row(Modifier.matchParentSize()) {
                    Box(Modifier.weight(1f).fillMaxHeight().clickable {
                        onRate(if (rating == halfValue) 0 else halfValue)
                    })
                    Box(Modifier.weight(1f).fillMaxHeight().clickable {
                        onRate(if (rating == fullValue) 0 else fullValue)
                    })
                }
            }
        }
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
                            for (starPos in 1..5) {
                                val halfValue = starPos * 2 - 1
                                val fullValue = starPos * 2
                                val icon = when {
                                    albumRating >= fullValue -> Icons.Default.Star
                                    albumRating >= halfValue -> Icons.AutoMirrored.Filled.StarHalf
                                    else -> Icons.Default.StarOutline
                                }
                                val filled = albumRating >= halfValue
                                Box(modifier = Modifier.size(36.dp)) {
                                    Icon(
                                        icon,
                                        contentDescription = null,
                                        tint = if (filled) Color(0xFFE6B422)
                                               else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
                                        modifier = Modifier.size(24.dp).align(Alignment.Center)
                                    )
                                    Row(Modifier.matchParentSize()) {
                                        Box(Modifier.weight(1f).fillMaxHeight().clickable {
                                            val v = halfValue; albumRating = if (albumRating == v) 0 else v
                                            vm.rateAlbumDirect(albumForRating, albumRating)
                                        })
                                        Box(Modifier.weight(1f).fillMaxHeight().clickable {
                                            val v = fullValue; albumRating = if (albumRating == v) 0 else v
                                            vm.rateAlbumDirect(albumForRating, albumRating)
                                        })
                                    }
                                }
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
