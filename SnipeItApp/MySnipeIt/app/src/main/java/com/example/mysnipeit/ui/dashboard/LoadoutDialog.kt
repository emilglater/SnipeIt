package com.example.mysnipeit.ui.dashboard

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.example.mysnipeit.data.models.BallisticProfiles
import com.example.mysnipeit.ui.theme.*

/**
 * Loadout picker — choose the cartridge and rifle profile the app-side
 * ballistic calculator will use. Opened from the dashboard MENU dialog.
 *
 * Two independent columns (any cartridge can be paired with any rifle for
 * what-if checks); a tap selects immediately and the choice is persisted
 * by the ViewModel, so there's no confirm step — CLOSE just dismisses.
 */
@Composable
fun LoadoutDialog(
    selectedCartridgeId: String,
    selectedRifleId: String,
    onCartridgeSelect: (String) -> Unit,
    onRifleSelect: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    val t = LocalTactical.current
    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Column(
            modifier = Modifier
                .width(responsiveDp(tablet = 720.dp, compact = 560.dp))
                .background(t.panel)
                .border(1.dp, t.lineHi)
                .padding(20.dp)
                .verticalScroll(rememberScrollState()),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Lbl(text = "LOADOUT · BALLISTIC PROFILE")
                Box(
                    modifier = Modifier
                        .border(1.dp, t.line)
                        .clickable(onClick = onDismiss)
                        .padding(horizontal = 10.dp, vertical = 4.dp),
                ) {
                    Text(
                        text = "CLOSE",
                        color = t.ink,
                        fontSize = 9.sp,
                        letterSpacing = 0.18.em,
                        fontFamily = JetBrainsMono,
                    )
                }
            }
            Spacer(Modifier.height(16.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Column(
                    modifier = Modifier.weight(1f),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Lbl(text = "CARTRIDGE")
                    BallisticProfiles.cartridges.forEach { c ->
                        ProfileRow(
                            title = c.displayName,
                            subtitle = String.format(
                                "BC %.3f · %.0f M/S · %.1f G",
                                c.ballisticCoefficientG1, c.muzzleVelocityMps, c.bulletMassGrams,
                            ),
                            isSelected = c.id == selectedCartridgeId,
                            onClick = { onCartridgeSelect(c.id) },
                        )
                    }
                }
                Column(
                    modifier = Modifier.weight(1f),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Lbl(text = "RIFLE")
                    BallisticProfiles.rifles.forEach { r ->
                        ProfileRow(
                            title = r.displayName,
                            subtitle = String.format(
                                "ZERO %.0f M · SIGHT %.0f MM",
                                r.zeroRangeM, r.sightHeightMm,
                            ),
                            isSelected = r.id == selectedRifleId,
                            onClick = { onRifleSelect(r.id) },
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun ProfileRow(
    title: String,
    subtitle: String,
    isSelected: Boolean,
    onClick: () -> Unit,
) {
    val t = LocalTactical.current
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(if (isSelected) t.panelHi else t.panel)
            .border(1.dp, if (isSelected) t.accent else t.line)
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 10.dp),
    ) {
        Text(
            text = title,
            color = if (isSelected) t.accent else t.ink,
            fontSize = 12.sp,
            letterSpacing = 0.06.em,
            fontFamily = JetBrainsMono,
        )
        Spacer(Modifier.height(4.dp))
        Text(
            text = subtitle,
            color = t.inkDim,
            fontSize = 9.sp,
            letterSpacing = 0.12.em,
            fontFamily = JetBrainsMono,
        )
    }
}
