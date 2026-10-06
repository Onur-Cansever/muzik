# muzik — endless generative music in pure C

Sonsuz, jeneratif müzik. Argümanlarla (bpm, seed) çalışır;
stdout'a ham mono PCM yazar; bir çalar canlı oynatır ya da bir dosyaya kaydedersin.
Kapatınca müzik biter. Daemon yok, audio kütüphanesi yok.

## Ne yapar

Dört katman üst üste biner:

1. **Key / tonlama** — A minör pentatonik. PRNG "hangi nota" değil,
   "ölçek içinden hangisi" sorar. Dışarı çıkamaz, her zaman uyumlu.
2. **Tempo** — deterministik beat clock. Rastgelelik tempo'ya karışmaz.
3. **Ritim** — 16 adımlık bar: kick, snare, hat, bass.
4. **Flow** — her 8 bar'da bir yeni bölüm: enerji + density değişir.

Ses tamamen **katılımsal sentez** (additive synthesis):

| Ses | Nasıl üretilir |
|---|---|
| Melodi / bas | Sinüs osilatörü + üstel genlik zarfı |
| Kick | Frekansı düşen sinüs (160→40 Hz) + sönüm |
| Hat | Beyaz gürültü (PRNG) + hızlı sönüm |
| Snare | Gürültü + 180 Hz sinüs karışımı |

Ses dosyası, sample, kütüphane yok. Her örnek o an hesaplanır.
PRNG hem rastgelelik hem gürültü kaynağı.

## Build

Herhangi bir GCC/Clang/LLD ile, tek satır:

```sh
cc -O2 -o muzik muzik.c -lm
```

Daha katı derlemek istersen (FreeBSD'de `cc` = Clang, aynı çalışır):

```sh
cc -O2 -std=c11 -pedantic -o muzik muzik.c -lm
```

Bağımlılık: sadece C stdlib + libm. Çalar (aplay/play/ffplay/ffmpeg)
sadece **oynatmak/kaydetmek** için gerekli; jeneratörün kendisi hiçbir
audio API'si kullanmaz, stdout'a yazar.

## Çıktı formatı

- 44100 Hz
- 16-bit signed integer
- **Mono** (tek kanal)
- Little-endian
- Ham raw PCM, header YOK

Format header'sız olduğu için çalar/kaydedici tarafında belirtilmesi
zorunlu (aşağıdaki komutlarda gösterildi).

## Oynat (canlı)

Üç seçenek — Linux + FreeBSD ikisinde de çalışır:

```sh
# FFmpeg (her iki platformda da aynı komut, önerilen)
./muzik | ffplay -nodisp -autoexit -f s16le -ar 44100 -i -

# Linux: ALSA
./muzik | aplay -f S16_LE -r 44100 -c 1

# FreeBSD: SoX (pkg install sox)
./muzik | play -t raw -e signed-integer -b 16 -r 44100 -c 1 -
```

Ctrl-C ile durdur; pipe kapanır, jeneratör sonraki `write`'ta çıkış yapar.

## Kaydet (dosyaya)

Jeneratör sonsuz ürettiği için kaydederken **süre limiti** vermek zorundasın
(`timeout` saniye cinsinden: 300 = 5 dakika).

### WAV olarak kaydet (önerilen, taşınabilir)

FFmpeg hem raw okur hem WAV header'ı ekler; komut her iki platformda da aynı:

```sh
timeout 300 ./muzik 120 42 | ffmpeg -y -f s16le -ar 44100 -i - kayit.wav
```

Sonuç: `kayit.wav` — herhangi bir çalarda, her sistemde açılır.

Alternatif — SoX ile (FreeBSD'de ffmpeg yoksa):

```sh
timeout 300 ./muzik 120 42 | sox -t raw -e signed-integer -b 16 -r 44100 -c 1 - kayit.wav
```

### Raw olarak kaydet (ham, hafif)

```sh
timeout 300 ./muzik 120 42 > kayit.raw
```

Bu dosya 44100 Hz / 16-bit / mono raw'dır. Oynatırken format belirt:

```sh
ffplay -nodisp -f s16le -ar 44100 -i kayit.raw
# ya da
play -t raw -e signed-integer -b 16 -r 44100 -c 1 kayit.raw
```

### Aynı anda hem dinle hem kaydet

```sh
timeout 300 ./muzik 120 42 | tee kayit.raw | ffplay -nodisp -autoexit -f s16le -ar 44100 -i -
```

(`tee` veriyi hem dosyaya hem çalara akıtır.)

## Argümanlar

```sh
./muzik [bpm] [seed]
```

| Argüman | Varsayılan | Açıklama |
|---|---|---|
| `bpm` | 120 | Tempo (40–240) |
| `seed` | saat'ten | PRNG tohumu. Aynı seed = aynı seans (tekrarlanabilir). |

### Önerilen preset (ilk deneme için)

```sh
./muzik 120 42
```

Bu kombinasyon ölçümle seçildi: RMS seviyesi tüm adaylar arasında en
yüksek (en "dolgun" his), clip oranı 0, ZCR dengeli. Sessiz ya da
aşırı parlak çıkmaz; standart 120 bpm'de.

Diğer denemeler:

```sh
./muzik 80 55      # yavaş, lo-fi his
./muzik 100 3      # orta tempo, hat yoğun
./muzik 110 12345  # hızlı, enerjik
./muzik            # rastgele seed, sürpriz
```

Aynı seed ile iki ayrı kayıt birebir aynı sesi verir —
"bugün güzel çalan seansı yeniden üretmek" için seed'i not et.

## Nasıl çalışır (kısa)

- **PRNG:** LCG. Seed verilmezse `clock_gettime(CLOCK_MONOTONIC)`'ten alınır.
- **Beat clock:** `beat_samples = 44100 * 60 / bpm`. Faz biriktiricisi
  (double) her örnekte ilerler; taşınca bir "step" tetiklenir.
- **Adım (16'lık):** step'e göre kick/snare/hat/bass/melodi kararları alınır.
- **Flow:** her 128 adım (8 bar) `new_section()` çağrılır → `energy`, `density`.
- **Sentez:** 16 slotluk ses havuzu. Her blokta aktif slotların toplamı
  hesaplanır, yumak (soft-clip) uygulanır, stdout'a yazılır.
- **Buffer:** 4410 örnek (0.1 sn) başına bir `write`. Pipe buffer'ı
  (~0.36 sn) underrun marjı verir; CPU yükü ihmal edilebilir.

## Genişletme noktaları

- **Key/scale değiştirmek:** `scale[]` dizisini değiştir (semitone listesi).
- **Desen ekle:** `step()` içinde `s`'e göre yeni vuruş/desen ekleyebilirsin.
- **Zenginleştirme:** `sample()` içinde ek katman (filtre, reverb, harmonik).
- **Stereo:** mono sinyalin kopyasını ikinci kanala da yaz (`buf` 2 kat
  büyüt, komutlarda `-c 1` → `-c 2`); ya da iki farklı sinyal üret.
- **Config dosyası:** argümanlar yerine dosya okumak istersen
  `main()`'da dosya parse'ı ekleyebilirsin.

## Notlar

- Sıcak döngüde malloc yok, I/O yok, hata mesajı yok.
- Sert real-time garantisi yok (normal kernel); ama 0.1 sn'lik buffer
  ve ~%0.01 CPU yüküyle underrun pratik olarak imkânsız.
- Kriptografik rastgelelik amaçlanmıyor; LCG yeterlidir.
- Streaming WAV denendi (RIFF header + sonsuz data): hem FFmpeg hem SoX
  bozuk header'ı reddetti; bu yüzden çıktı ham raw, header çalar tarafında.
