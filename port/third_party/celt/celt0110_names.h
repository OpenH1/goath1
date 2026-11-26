/* CELT 0.11.0's external names, each given the prefix celt0110_ so that they do not meet those of Opus's CELT
(port/third_party/opus), which has many of the same. Included before everything else in CELT's own files (the
build's -include) and before celt.h by its callers. Made from `nm -g --defined-only` of CELT's objects. */

#ifndef CELT0110_NAMES_H
#define CELT0110_NAMES_H

/* (glibc's, which the guest's C library has not: without it ecintrin.h's EC_ILOG falls back to ec_ilog, which
entcode.h uses before it declares it) */
#if defined(__GNUC__) && !defined(__GNUC_PREREQ)
#define __GNUC_PREREQ(major, minor) ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((major) << 16) + (minor))
#endif

#define alg_quant celt0110_alg_quant
#define alg_unquant celt0110_alg_unquant
#define amp2Log2 celt0110_amp2Log2
#define anti_collapse celt0110_anti_collapse
#define _celt_autocorr celt0110__celt_autocorr
#define celt_decode celt0110_celt_decode
#define celt_decode_float celt0110_celt_decode_float
#define celt_decoder_create celt0110_celt_decoder_create
#define celt_decoder_create_custom celt0110_celt_decoder_create_custom
#define celt_decoder_ctl celt0110_celt_decoder_ctl
#define celt_decoder_destroy celt0110_celt_decoder_destroy
#define celt_decoder_get_size celt0110_celt_decoder_get_size
#define celt_decoder_get_size_custom celt0110_celt_decoder_get_size_custom
#define celt_decoder_init celt0110_celt_decoder_init
#define celt_decoder_init_custom celt0110_celt_decoder_init_custom
#define celt_encode celt0110_celt_encode
#define celt_encode_float celt0110_celt_encode_float
#define celt_encoder_create celt0110_celt_encoder_create
#define celt_encoder_create_custom celt0110_celt_encoder_create_custom
#define celt_encoder_ctl celt0110_celt_encoder_ctl
#define celt_encoder_destroy celt0110_celt_encoder_destroy
#define celt_encoder_get_size celt0110_celt_encoder_get_size
#define celt_encoder_get_size_custom celt0110_celt_encoder_get_size_custom
#define celt_encoder_init celt0110_celt_encoder_init
#define celt_encoder_init_custom celt0110_celt_encoder_init_custom
#define _celt_lpc celt0110__celt_lpc
#define celt_mode_create celt0110_celt_mode_create
#define celt_mode_destroy celt0110_celt_mode_destroy
#define celt_mode_info celt0110_celt_mode_info
#define celt_strerror celt0110_celt_strerror
#define clt_mdct_backward celt0110_clt_mdct_backward
#define clt_mdct_clear celt0110_clt_mdct_clear
#define clt_mdct_forward celt0110_clt_mdct_forward
#define clt_mdct_init celt0110_clt_mdct_init
#define compute_allocation celt0110_compute_allocation
#define compute_band_energies celt0110_compute_band_energies
#define compute_pulse_cache celt0110_compute_pulse_cache
#define decode_pulses celt0110_decode_pulses
#define denormalise_bands celt0110_denormalise_bands
#define ec_dec_bit_logp celt0110_ec_dec_bit_logp
#define ec_dec_bits celt0110_ec_dec_bits
#define ec_dec_icdf celt0110_ec_dec_icdf
#define ec_dec_init celt0110_ec_dec_init
#define ec_decode celt0110_ec_decode
#define ec_decode_bin celt0110_ec_decode_bin
#define ec_dec_uint celt0110_ec_dec_uint
#define ec_dec_update celt0110_ec_dec_update
#define ec_enc_bit_logp celt0110_ec_enc_bit_logp
#define ec_enc_bits celt0110_ec_enc_bits
#define ec_enc_done celt0110_ec_enc_done
#define ec_enc_icdf celt0110_ec_enc_icdf
#define ec_enc_init celt0110_ec_enc_init
#define ec_encode celt0110_ec_encode
#define ec_encode_bin celt0110_ec_encode_bin
#define ec_enc_shrink celt0110_ec_enc_shrink
#define ec_enc_uint celt0110_ec_enc_uint
#define ec_ilog celt0110_ec_ilog
#define ec_laplace_decode celt0110_ec_laplace_decode
#define ec_laplace_encode celt0110_ec_laplace_encode
#define ec_tell_frac celt0110_ec_tell_frac
#define encode_pulses celt0110_encode_pulses
#define fir celt0110_fir
#define get_required_bits celt0110_get_required_bits
#define haar1 celt0110_haar1
#define icwrs celt0110_icwrs
#define iir celt0110_iir
#define isqrt32 celt0110_isqrt32
#define kiss_fft celt0110_kiss_fft
#define kiss_fft_alloc celt0110_kiss_fft_alloc
#define kiss_fft_alloc_twiddles celt0110_kiss_fft_alloc_twiddles
#define kiss_fft_free celt0110_kiss_fft_free
#define kiss_ifft celt0110_kiss_ifft
#define lcg_rand celt0110_lcg_rand
#define log2Amp celt0110_log2Amp
#define log2_frac celt0110_log2_frac
#define normalise_bands celt0110_normalise_bands
#define pitch_downsample celt0110_pitch_downsample
#define pitch_search celt0110_pitch_search
#define quant_all_bands celt0110_quant_all_bands
#define quant_coarse_energy celt0110_quant_coarse_energy
#define quant_energy_finalise celt0110_quant_energy_finalise
#define quant_fine_energy celt0110_quant_fine_energy
#define remove_doubling celt0110_remove_doubling
#define renormalise_vector celt0110_renormalise_vector
#define spreading_decision celt0110_spreading_decision
#define stereo_itheta celt0110_stereo_itheta
#define unquant_coarse_energy celt0110_unquant_coarse_energy
#define unquant_energy_finalise celt0110_unquant_energy_finalise
#define unquant_fine_energy celt0110_unquant_fine_energy

#endif
