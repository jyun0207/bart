/* Copyright 2022-2023. TU Graz. Institute of Biomedical Imaging.
 * All rights reserved. Use of this source code is governed by
 * a BSD-style license which can be found in the LICENSE file.
 */

#include <math.h>
#include <assert.h>

#include "misc/misc.h"
#include "misc/types.h"

#include "num/specfun.h"

#include "pulse.h"

DEF_TYPEID(pulse_sinc);
DEF_TYPEID(pulse_rect);

extern inline complex float pulse_eval(const struct pulse* p, float t);


/* windowed sinc pulse */

static float sinc_windowed(float alpha, float t, float n)
{
	return ((1. - alpha) + alpha * cosf(M_PI * t / n)) * sincf(M_PI * t);
}

/* analytical integral of windowed sinc */

static float sinc_windowed_antiderivative(float alpha, float t, float n)
{
	return (alpha * (Si(M_PI * t * (n + 1.) / n) + Si(M_PI * t * (n - 1.) / n))
			- 2. * (alpha - 1.) * Si(M_PI * t)) / (2. * M_PI);
}



// Analytical definition of windowed sinc pulse
// 	! centered around 0
// 	-> Shift by half of pulse length to start pulse at t=0
float pulse_sinc(const struct pulse_sinc* ps, float t)
{
	float mid = CAST_UP(ps)->duration / 2.;
	float t0 = CAST_UP(ps)->duration / ps->bwtp;

	assert((0 <= t) && (t <= CAST_UP(ps)->duration));

	return ps->A * sinc_windowed(ps->alpha, (t - mid) / t0, ps->bwtp / 2.);
}

float pulse_sinc_integral(const struct pulse_sinc* ps)
{
	float mid = CAST_UP(ps)->duration / 2.;
	float t0 = CAST_UP(ps)->duration / ps->bwtp;

	return ps->A * t0 * (sinc_windowed_antiderivative(ps->alpha, +mid / t0, ps->bwtp / 2.)
			- sinc_windowed_antiderivative(ps->alpha, -mid / t0, ps->bwtp / 2.));
}


static complex float pulse_sinc_eval(const struct pulse* _ps, float t)
{
	auto ps = CAST_DOWN(pulse_sinc, _ps);

	return pulse_sinc(ps, t);
}


const struct pulse_sinc pulse_sinc_defaults = {

	.INTERFACE.duration = 0.001,
	.INTERFACE.flipangle = 1.,
	.INTERFACE.eval = pulse_sinc_eval,
	.INTERFACE.TYPEID = &TYPEID2(pulse_sinc),
	// .pulse.phase = 0.,

	.alpha = 0.46,
	.A = 1.,
	.bwtp = 4.,
};


// Assume symmetric windowed sinc pulses
// 	- Ensure windowed sinc leads to 90 deg rotation if its integral is pi/2
void pulse_sinc_init(struct pulse_sinc* ps, float duration, float angle /*[deg]*/, float phase, float bwtp, float alpha)
{
	ps->INTERFACE.duration = duration;
	ps->INTERFACE.flipangle = angle;
	ps->INTERFACE.eval = pulse_sinc_eval;
	(void)phase;
//	ps->pulse.phase = phase;

	ps->bwtp = bwtp;
	ps->alpha = alpha;
	ps->A = 1.;

	float integral = pulse_sinc_integral(ps);
	float scaling = M_PI / 2. / integral;

	ps->A = scaling / 90. * angle;
}


/* Rectangular pulse */

void pulse_rect_init(struct pulse_rect* pr, float duration, float angle /*[deg]*/, float phase)
{
	pr->INTERFACE.duration = duration;
	pr->INTERFACE.flipangle = angle;
	(void)phase;
//	pulse->phase = phase;		// [rad]

	pr->A = angle / duration * M_PI / 180.;
}

float pulse_rect(const struct pulse_rect* pr, float t)
{
	(void)t;
	return pr->A;
}

static complex float pulse_rect_eval(const struct pulse* _pr, float t)
{
	auto pr = CAST_DOWN(pulse_rect, _pr);

	return pulse_rect(pr, t);
}

const struct pulse_rect pulse_rect_defaults = {

	.INTERFACE.duration = 0.001,
	.INTERFACE.flipangle = 1.,
	.INTERFACE.eval = pulse_rect_eval,
	.INTERFACE.TYPEID = &TYPEID2(pulse_rect),
	// .pulse.phase = 0.,

	.A = 1.,
};


/* Hyperbolic Secant Pulse
 *
 * Baum J, Tycko R, Pines A.
 * Broadband and adiabatic inversion of a two-level system by phase-modulated pulses.
 * Phys Rev A 1985;32:3435-3447.
 *
 * Bernstein MA, King KF, Zhou XJ.
 * Handbook of MRI Pulse Sequences.
 * Chapter 6
 */

const struct pulse_hypsec pulse_hypsec_defaults = {

	.INTERFACE.duration = 0.01,
	.INTERFACE.flipangle = 180.,
//	.pulse.phase = 0.,

	.a0 = 13000.,
	.beta = 800.,
	.mu = 4.9, /* sech(x)=0.01*/
};

static float sechf(float x)
{
	return 1. / coshf(x);
}

float pulse_hypsec_am(const struct pulse_hypsec* hs, float t /*[s]*/)
{
        // Check adiabatic condition
        assert(hs->a0 > sqrtf(hs->mu) * hs->beta);

        return hs->a0 * sechf(hs->beta * (t - CAST_UP(hs)->duration / 2.));
}

float pulse_hypsec_fm(const struct pulse_hypsec* hs, float t /*[s]*/)
{
	return -hs->mu * hs->beta * tanhf(hs->beta * (t - CAST_UP(hs)->duration / 2.));
}

float pulse_hypsec_phase(const struct pulse_hypsec* hs, float t /*[s]*/)
{
        return hs->mu * logf(sechf(hs->beta * (t - CAST_UP(hs)->duration / 2.)))
                + hs->mu * logf(hs->a0);
}

