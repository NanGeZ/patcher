package top.nkbe.npatch.ui.component

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.RowScope
import androidx.compose.runtime.Composable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import dev.chrisbanes.haze.HazeState
import dev.chrisbanes.haze.blur.HazeBlurStyle
import dev.chrisbanes.haze.blur.blurEffect
import dev.chrisbanes.haze.hazeEffect
import top.yukonga.miuix.kmp.basic.ScrollBehavior
import top.yukonga.miuix.kmp.basic.TopAppBar
import top.yukonga.miuix.kmp.basic.TopAppBarDefaults
import top.yukonga.miuix.kmp.theme.MiuixTheme

@Composable
fun NPatchTopAppBar(
    title: String,
    modifier: Modifier = Modifier,
    color: Color = Color.Transparent,
    titleColor: Color = MiuixTheme.colorScheme.onSurface,
    largeTitle: String = title,
    largeTitleColor: Color = MiuixTheme.colorScheme.onSurface,
    subtitle: String = "",
    subtitleColor: Color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
    navigationIcon: @Composable () -> Unit = {},
    actions: @Composable RowScope.() -> Unit = {},
    scrollBehavior: ScrollBehavior? = null,
    defaultWindowInsetsPadding: Boolean = true,
    titlePadding: Dp = TopAppBarDefaults.TitlePadding,
    navigationIconPadding: Dp = TopAppBarDefaults.NavigationIconPadding,
    actionIconPadding: Dp = TopAppBarDefaults.ActionIconPadding,
    bottomContent: @Composable () -> Unit = {},
    hazeState: HazeState? = null,
    hazeStyle: HazeBlurStyle? = null,
    blurVisible: Boolean? = null,
) {
    val isScrolled by remember(scrollBehavior) {
        derivedStateOf {
            scrollBehavior?.state?.collapsedFraction?.let { it > 0.01f } ?: true
        }
    }
    val effectiveBlurVisible = blurVisible ?: (scrollBehavior == null || isScrolled)
    val hazeModifier = if (effectiveBlurVisible && hazeState != null && hazeStyle != null) {
        Modifier.hazeEffect(hazeState) {
            blurEffect {
                style = hazeStyle
                blurRadius = 30.dp
                noiseFactor = 0f
            }
        }
    } else {
        Modifier
    }

    Box(modifier = modifier.then(hazeModifier)) {
        TopAppBar(
            title = title,
            modifier = Modifier,
            color = color,
            titleColor = titleColor,
            largeTitle = largeTitle,
            largeTitleColor = largeTitleColor,
            subtitle = subtitle,
            subtitleColor = subtitleColor,
            navigationIcon = navigationIcon,
            actions = actions,
            scrollBehavior = scrollBehavior,
            defaultWindowInsetsPadding = defaultWindowInsetsPadding,
            titlePadding = titlePadding,
            navigationIconPadding = navigationIconPadding,
            actionIconPadding = actionIconPadding,
            bottomContent = bottomContent,
        )
    }
}
