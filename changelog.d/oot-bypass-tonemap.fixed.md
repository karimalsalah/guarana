- Ocarina of Time with No sharpening, the default on Intel integrated GPUs: the picture is no
  longer hazier and flatter than in v20.4.0. Since v20.5.0's upstream merge the bypass copied the
  image the network's output pass samples, which on Hyrule Field is not the tonemapped picture
  (26 dB from the network's own output, against 32 dB). It copies the tonemap drawn that frame
  again, and the sampled image only when no known tonemap was drawn (Lichii #1688's case).
