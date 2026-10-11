/// <reference path="./.config/sa.d.ts" />
// e2e_modern.py (copied into CLEO\ for the run): Grove Street cul-de-sac, EXTRASUNNY_LA. The clock is held (re-set
// every 0.5 s) at 13:00 from +20 s to +90 s (the template calibrates on daytime), then at 17:30, so the shots don't
// depend on when the script started.
wait(20000);
Weather.ForceNow(0);
new Player(0).getChar().setCoordinates(2495.0, -1685.0, 13.5);
for (let i = 0; i < 140; ++i) { Clock.SetTimeOfDay(13, 0); wait(500); }
while (true) { Clock.SetTimeOfDay(17, 30); wait(500); }
