auto HDPackBuilder::create() -> void {
  setCollapsible();
  setVisible(false);

  headerLabel.setText("HD Pack Builder").setFont(Font().setBold());
  headerSpacer.setColor({192, 192, 192});

  useHDPackToggle.setText("Use HD packs")
    .setChecked(settings.emulator.hack.ppu.useHDPack)
    .onToggle([&] {
      settings.emulator.hack.ppu.useHDPack = useHDPackToggle.checked();
      emulator->configure("Hacks/PPU/UseHDPack", settings.emulator.hack.ppu.useHDPack);
    });

  hdTileDumpToggle.setText("Dump HD tiles")
    .setChecked(settings.emulator.hack.ppu.hdTileDump)
    .onToggle([&] {
      settings.emulator.hack.ppu.hdTileDump = hdTileDumpToggle.checked();
      emulator->configure("Hacks/PPU/HDTileDump", settings.emulator.hack.ppu.hdTileDump);
    });

  infoView.setText(
    "This tool helps with building HD packs.\n\n"
    "- Toggle 'Use HD packs' to enable loading HD packs at runtime.\n"
    "- Toggle 'Dump HD tiles' to export tiles for creating/updating packs.\n\n"
    "Future versions may add pack creation and file management here."
  );
}
