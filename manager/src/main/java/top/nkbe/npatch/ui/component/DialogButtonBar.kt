package top.nkbe.npatch.ui.component

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.Column
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import top.yukonga.miuix.kmp.basic.ButtonDefaults
import top.yukonga.miuix.kmp.basic.CircularProgressIndicator
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.basic.TextButton
import top.yukonga.miuix.kmp.overlay.OverlayDialog
import top.yukonga.miuix.kmp.theme.MiuixTheme

class DialogButtonBarAction(
    val text: String,
    val onClick: () -> Unit,
)

/** 對話框底部按鈕列：neutral 在上方整行，negative / positive 並排。 */
@Composable
fun DialogButtonBar(
    positive: DialogButtonBarAction,
    modifier: Modifier = Modifier,
    negative: DialogButtonBarAction? = null,
    neutral: DialogButtonBarAction? = null,
) {
    Column(modifier = modifier.fillMaxWidth()) {
        if (neutral != null) {
            TextButton(
                text = neutral.text,
                onClick = neutral.onClick,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(8.dp))
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            if (negative != null) {
                TextButton(
                    text = negative.text,
                    onClick = negative.onClick,
                    modifier = Modifier.weight(1f),
                )
            }
            TextButton(
                text = positive.text,
                onClick = positive.onClick,
                modifier = Modifier.weight(1f),
                colors = ButtonDefaults.textButtonColorsPrimary(),
            )
        }
    }
}

@Composable
fun OverlayLoadingDialog(
    text: String,
    show: Boolean,
    onDismissRequest: () -> Unit,
    renderInRootScaffold: Boolean = true,
) {
    OverlayDialog(
        show = show,
        onDismissRequest = onDismissRequest,
        renderInRootScaffold = renderInRootScaffold,
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            CircularProgressIndicator()
            Spacer(Modifier.width(16.dp))
            Text(text = text, color = MiuixTheme.colorScheme.onSurface)
        }
    }
}
