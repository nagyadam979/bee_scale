/*
 * sens_vbat.h — akkufeszültség mérés: NPN-kapcsolt feszültségosztó + ADC1
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * A TP4056 OUT+ (akkusín, 4.2..3.0 V) feszültségét egy 10k/10k osztó felezi az ADC1 (GPIO0)
 * bemenetre. Az osztó alsó ágának GND-jét egy NPN (bázis=GPIO10, 10k) kapcsolja: alvásban/mérésen
 * kívül LOW -> ~0 szivárgás; méréskor HIGH -> VBAT_SETTLE_MS beállás -> ADC átlag.
 * A leolvasott mV-t a VBAT_DIVIDER_RATIO-val visszaszorozzuk az akku mV-jára.
 */
#ifndef SENS_VBAT_H
#define SENS_VBAT_H

#include <stdint.h>

/* GPIO (NPN-kapcsoló) + ADC1 oneshot + kalibráció init. Egyszer, boot után. */
void vbat_init(void);

/* NPN ON: az osztó él (mérés-fázis kezdete). A stabil ADC-hez VBAT_SETTLE_MS várakozás kell utána. */
void vbat_enable(void);

/* NPN OFF: az osztó lekapcsol (~0 szivárgás). Mérés után hívandó. */
void vbat_disable(void);

/*
 * TISZTA ADC-minta mV-ban: VBAT_ADC_SAMPLES átlag + osztó-visszaszorzás. FELTÉTELEZI, hogy az
 * osztó már bekapcsolt ÉS beállt (a hívó előtte vbat_enable + VBAT_SETTLE_MS várt). A BENCH
 * VBAT-fázis ezt hívja ciklikusan, végig bekapcsolt osztóval (folyamatos DMM-mérés).
 */
uint16_t vbat_sample_mv(void);

/*
 * Egy komplett akkufeszültség-leolvasás mV-ban: enable -> settle -> vbat_sample_mv -> disable.
 * A FIELD profil ezt hívja (egyszeri mérés). A payload vbat (uint16_t mV) mezőjébe való.
 */
uint16_t vbat_read_mv(void);

/*
 * Deep sleep előtti fogyasztáscsökkentés: ADC unit/kalibráció elengedése és osztó OFF.
 * Deep sleep után úgyis reboot jön, ezért nem kell visszaépíthető állapotot tartani.
 */
void vbat_prepare_sleep(void);

#endif /* SENS_VBAT_H */
