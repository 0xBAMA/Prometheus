/*
 * SPDX-FileCopyrightText: Copyright (c) <2023> NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * [Jendersie and d'Eon 2023]
 * SIGGRAPH 2023 Talks
 *
 * Approximate Mie phase function for fog/cloud rendering.
 *
 * Large-particle parameterization:
 *     5um <= d <= 50um
 *
 * Supplemental, Section 3.1, Eqs. 7-10.
 */

// ============================================================================
// Henyey-Greenstein
// ============================================================================

// Evaluate the Henyey-Greenstein phase function.
//
// u = cos(theta) = dot(incoming_direction, outgoing_direction)
// g = HG asymmetry parameter
//
float evalHG(in float u, in float g)
{
    float g2 = g * g;

    return (1.0 - g2) /
           (4.0 * pi * pow(1.0 + g2 - 2.0 * g * u, 1.5));
}


// Sample an HG deflection cosine.
//
// xi = uniform random number in [0,1)
// g  = HG asymmetry parameter
//
// From Eq. 3 of the supplemental.
//
float sampleHGcos(in float xi, in float g)
{
    // Isotropic fallback.
    if (abs(g) < 1e-4)
        return 2.0 * xi - 1.0;

    float g2 = g * g;

    float t = (1.0 - g2) /
              (1.0 - g + 2.0 * g * xi);

    float u = (1.0 + g2 - t * t) /
              (2.0 * g);

    return clamp(u, -1.0, 1.0);
}


// ============================================================================
// Draine
// ============================================================================

// Evaluate the Draine phase function.
//
// g = Draine asymmetry parameter
// a = alpha shape parameter
// u = cos(theta)
//
float evalDraine(in float u, in float g, in float a)
{
    float g2 = g * g;

    return ((1.0 - g2) * (1.0 + a * u * u)) /
           (4.0 *
            (1.0 + a * (1.0 + 2.0 * g2) / 3.0) *
            pi *
            pow(1.0 + g2 - 2.0 * g * u, 1.5));
}


// Sample an exact Draine deflection cosine.
//
// xi = uniform random number in [0,1)
// g  = Draine asymmetry parameter
// a  = alpha
//
// This is the exact inversion from Eq. 6 of the supplemental.
//
float sampleDraineCos(in float xi, in float g, in float a)
{
    // Isotropic fallback.
    if (abs(g) < 1e-4)
        return 2.0 * xi - 1.0;

    const float g2 = g * g;
    const float g3 = g * g2;
    const float g4 = g2 * g2;
    const float g6 = g2 * g4;

    const float pgp1_2 = (1.0 + g2) * (1.0 + g2);

    const float T1 =
        (-1.0 + g2) *
        (4.0 * g2 + a * pgp1_2);

    const float T1a =
        -a + a * g4;

    const float T1a3 =
        T1a * T1a * T1a;

    const float T2 =
        -1296.0 *
        (-1.0 + g2) *
        (a - a * g2) *
        T1a *
        (4.0 * g2 + a * pgp1_2);

    const float T3 =
        3.0 * g2 * (1.0 + g * (-1.0 + 2.0 * xi)) +
        a * (
            2.0 +
            g2 +
            g3 * (1.0 + 2.0 * g2) * (-1.0 + 2.0 * xi)
        );

    const float T4a =
        432.0 * T1a3 +
        T2 +
        432.0 * (a - a * g2) * T3 * T3;

    const float T4b =
        -144.0 * a * g2 +
        288.0 * a * g4 -
        144.0 * a * g6;

    const float T4b3 =
        T4b * T4b * T4b;

    const float discriminant =
        max(0.0, -4.0 * T4b3 + T4a * T4a);

    const float T4 =
        T4a + sqrt(discriminant);

    // Avoid a zero/negative cube-root argument.
    const float T4safe =
        (abs(T4) < 1e-8) ? 1e-8 : T4;

    const float T4p3 =
        pow(T4safe, 1.0 / 3.0);

    const float T6 =
        (
            2.0 * T1a +
            (
                48.0 * pow(2.0, 1.0 / 3.0) *
                (-(a * g2) + 2.0 * a * g4 - a * g6)
            ) / T4p3 +
            T4p3 / (3.0 * pow(2.0, 1.0 / 3.0))
        ) /
        (a - a * g2);

    const float T5 =
        6.0 * (1.0 + g2) + T6;

    const float sqrtT5 =
        sqrt(max(T5, 1e-8));

    float inner =
        6.0 * (1.0 + g2) -
        (8.0 * T3) /
        (a * (-1.0 + g2) * sqrtT5) -
        T6;

    inner = max(inner, 0.0);

    float u =
        (
            1.0 + g2 -
            pow(
                -0.5 * sqrtT5 +
                0.5 * sqrt(inner),
                2.0
            )
        ) /
        (2.0 * g);

    return clamp(u, -1.0, 1.0);
}


// ============================================================================
// Large-particle Mie parameterization
// ============================================================================
//
// Section 3.1 of the supplemental:
//
//   gHG(d) = exp(-0.0990567 / (d - 1.67154))
//
//   gD(d)  = exp(-2.20679 / (d + 3.91029)) - 0.428934
//
//   a(d)   = exp(3.62489 - 8.29288 / (d + 5.52825))
//
//   wD(d)  = exp(-0.599085 / (d - 0.641583)) - 0.665888
//
// d is particle DIAMETER in micrometers.
//
// Valid range:
//
//   5 <= d <= 50
//
// ============================================================================

float mieLarge_gHG(in float d)
{
    d = clamp(d, 5.0, 50.0);

    return exp(
        -0.0990567 / (d - 1.67154)
    );
}


float mieLarge_gD(in float d)
{
    d = clamp(d, 5.0, 50.0);

    return exp(
        -2.20679 / (d + 3.91029)
    ) - 0.428934;
}


float mieLarge_alpha(in float d)
{
    d = clamp(d, 5.0, 50.0);

    return exp(
        3.62489 -
        8.29288 / (d + 5.52825)
    );
}


float mieLarge_wD(in float d)
{
    d = clamp(d, 5.0, 50.0);

    return exp(
        -0.599085 / (d - 0.641583)
    ) - 0.665888;
}


// ============================================================================
// Get all Mie parameters for a particle diameter.
// ============================================================================

void getMieParameters(
    in  float d,
    out float gHG,
    out float gD,
    out float alpha,
    out float wD)
{
    d = clamp(d, 5.0, 50.0);

    gHG   = mieLarge_gHG(d);
    gD    = mieLarge_gD(d);
    alpha = mieLarge_alpha(d);
    wD    = mieLarge_wD(d);
}


// ============================================================================
// Approximate Mie phase function
// ============================================================================
//
// phi_Mie = (1-wD) * phi_HG + wD * phi_Draine
//
// u = dot(rayDir, scatteredDir)
// d = particle diameter in micrometers
//
// ============================================================================

float evalApproxMie(
    in float u,
    in float d)
{
    float gHG;
    float gD;
    float alpha;
    float wD;

    getMieParameters(
        d,
        gHG,
        gD,
        alpha,
        wD
    );

    float hg =
        evalHG(u, gHG);

    float draine =
        evalDraine(
            u,
            gD,
            alpha
        );

    return
        (1.0 - wD) * hg +
        wD * draine;
}


// ============================================================================
// Build an orthonormal coordinate frame around a ray direction.
// ============================================================================
//
// Given:
//
//     forward = existing ray direction
//
// Produces:
//
//     tangent
//     bitangent
//
// such that:
//
//     tangent  x bitangent = forward
//
// ============================================================================

void makeBasis(
    in  vec3 forward,
    out vec3 tangent,
    out vec3 bitangent)
{
    forward = normalize(forward);

    // Pick the axis least parallel to forward.
    //
    // This avoids numerical instability when forward is nearly parallel
    // to the chosen reference axis.

    if (abs(forward.z) < 0.999)
    {
        tangent =
            normalize(
                cross(
                    vec3(0.0, 0.0, 1.0),
                    forward
                )
            );
    }
    else
    {
        tangent =
            normalize(
                cross(
                    vec3(0.0, 1.0, 0.0),
                    forward
                )
            );
    }

    bitangent =
        cross(
            forward,
            tangent
        );
}


// ============================================================================
// Convert a sampled spherical direction into world space.
// ============================================================================
//
// cosTheta = cos(theta)
// phi      = azimuth
// forward  = original ray direction
//
// ============================================================================

vec3 directionFromCosTheta(
    in vec3 forward,
    in float cosTheta,
    in float phi)
{
    vec3 tangent;
    vec3 bitangent;

    makeBasis(
        forward,
        tangent,
        bitangent
    );

    float sinTheta =
        sqrt(
            max(
                0.0,
                1.0 - cosTheta * cosTheta
            )
        );

    float cosPhi = cos(phi);
    float sinPhi = sin(phi);

    return normalize(
        forward * cosTheta +
        tangent * (sinTheta * cosPhi) +
        bitangent * (sinTheta * sinPhi)
    );
}


// ============================================================================
// Sample the approximate Mie phase function.
//
// rayDir     = existing incoming ray direction
// d          = particle diameter in micrometers
//
// xiLobe     = random number used to choose HG vs Draine
// xiTheta    = random number used for cosine sampling
// xiPhi      = random number used for azimuth
//
// Returns a NEW scattered ray direction.
//
// ============================================================================

vec3 sampleApproxMieDirection(
    in vec3 rayDir,
    in float d,
    in float xiLobe,
    in float xiTheta,
    in float xiPhi)
{
    float gHG;
    float gD;
    float alpha;
    float wD;

    getMieParameters(
        d,
        gHG,
        gD,
        alpha,
        wD
    );

    // ------------------------------------------------------------------------
    // Select which lobe to sample.
    //
    // Draine is selected when:
    //
    //     xiLobe < wD
    //
    // Otherwise HG is selected.
    //
    // This follows Section 2 of the supplemental.
    // ------------------------------------------------------------------------

    float u;

    if (xiLobe < wD)
    {
        // Draine lobe.
        //
        // Rescale xiLobe so it can be reused as the sampling random number.
        float xi =
            xiLobe / max(wD, 1e-6);

        u =
            sampleDraineCos(
                xi,
                gD,
                alpha
            );
    }
    else
    {
        // HG lobe.
        //
        // Rescale the random number into [0,1).
        float xi =
            (xiLobe - wD) /
            max(1.0 - wD, 1e-6);

        u =
            sampleHGcos(
                xi,
                gHG
            );
    }

    // Uniform azimuth.
    float phi =
        2.0 * pi * xiPhi;

    // Transform the sampled local direction into world space
    // around the original ray direction.
    return directionFromCosTheta(
        normalize(rayDir),
        u,
        phi
    );
}