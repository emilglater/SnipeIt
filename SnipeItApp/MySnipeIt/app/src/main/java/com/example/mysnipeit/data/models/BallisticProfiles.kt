package com.example.mysnipeit.data.models

/**
 * Cartridge ballistic profile — the inputs the ballistic solver needs to
 * trace a bullet's path. Kept deliberately minimal (G1 model only) so the
 * upcoming solver stays a simple closed-form-ish calculation rather than
 * a full 6-DOF integrator.
 *
 * Units:
 *  - [bulletMassGrams] grams (SI; matches the rest of the app)
 *  - [ballisticCoefficientG1] dimensionless, G1 reference drag
 *  - [muzzleVelocityMps] meters per second, measured at the muzzle for
 *    the rifle/barrel the cartridge is expected to be paired with
 *
 * Why G1 and not G7: G7 is more accurate for boat-tail spitzer bullets
 * but most published BC data the user is likely to know is G1. G1 is good
 * enough for a POC display; we can add G7 as an alternate column later.
 */
data class CartridgeProfile(
    val id: String,
    val displayName: String,
    val bulletMassGrams: Double,
    val ballisticCoefficientG1: Double,
    val muzzleVelocityMps: Double,
)

/**
 * Rifle profile — the rifle/optic geometry the solver needs on top of the
 * cartridge to produce a usable firing solution at a chosen range.
 *
 * Units:
 *  - [sightHeightMm] millimeters above the bore axis to the optical axis;
 *    typical sniper rifles are 38-50mm, carbines with raised optics are
 *    60-70mm. Sight height changes near-range trajectory more than far.
 *  - [zeroRangeM] the range (meters) the rifle is zeroed at. We assume a
 *    standard "single-point zero" — the bullet crosses the line of sight
 *    once at this range. The solver back-computes the sight angle from it.
 *
 * Cartridge is intentionally NOT bound to a rifle — the user picks both
 * independently from the dashboard menu, so a 7.62 rifle "profile" can be
 * paired with a non-standard cartridge for what-if calculations. The
 * `displayName` just hints at the rifle's intended caliber for the picker.
 */
data class RifleProfile(
    val id: String,
    val displayName: String,
    val sightHeightMm: Double,
    val zeroRangeM: Double,
)

/**
 * Standard preset catalog. Acts as the source of truth for the dashboard
 * menu pickers and the default selection when no user choice is persisted.
 *
 * BC and muzzle-velocity numbers are nominal published values for the
 * cartridges' most common loadings — close enough for a POC firing
 * solution, not field-truth data. A real deployment would let the user
 * enter chronograph numbers + measured BC; that's a later branch.
 */
object BallisticProfiles {

    val cartridges: List<CartridgeProfile> = listOf(
        CartridgeProfile(
            id = "762x51_m80",
            displayName = "7.62×51 NATO (M80, 147 gr)",
            bulletMassGrams = 9.53,
            ballisticCoefficientG1 = 0.395,
            muzzleVelocityMps = 838.0,
        ),
        CartridgeProfile(
            id = "556x45_m855",
            displayName = "5.56×45 NATO (M855, 62 gr)",
            bulletMassGrams = 4.02,
            ballisticCoefficientG1 = 0.304,
            muzzleVelocityMps = 948.0,
        ),
        CartridgeProfile(
            id = "300_win_mag",
            displayName = ".300 Win Mag (190 gr Match)",
            bulletMassGrams = 12.31,
            ballisticCoefficientG1 = 0.533,
            muzzleVelocityMps = 860.0,
        ),
        CartridgeProfile(
            id = "338_lapua_mag",
            displayName = ".338 Lapua Mag (250 gr Scenar)",
            bulletMassGrams = 16.20,
            ballisticCoefficientG1 = 0.675,
            muzzleVelocityMps = 880.0,
        ),
    )

    val rifles: List<RifleProfile> = listOf(
        RifleProfile(
            id = "m24_sws",
            displayName = "M24 SWS (7.62)",
            sightHeightMm = 38.0,
            zeroRangeM = 100.0,
        ),
        RifleProfile(
            id = "m110_sass",
            displayName = "M110 SASS (7.62)",
            sightHeightMm = 44.0,
            zeroRangeM = 100.0,
        ),
        RifleProfile(
            id = "m4_carbine",
            displayName = "M4 Carbine (5.56)",
            sightHeightMm = 65.0,
            zeroRangeM = 50.0,
        ),
        RifleProfile(
            id = "awm_338",
            displayName = "AWM (.338 LM)",
            sightHeightMm = 50.0,
            zeroRangeM = 100.0,
        ),
    )

    val defaultCartridge: CartridgeProfile = cartridges.first()
    val defaultRifle: RifleProfile = rifles.first()

    fun cartridgeById(id: String?): CartridgeProfile =
        cartridges.firstOrNull { it.id == id } ?: defaultCartridge

    fun rifleById(id: String?): RifleProfile =
        rifles.firstOrNull { it.id == id } ?: defaultRifle
}
