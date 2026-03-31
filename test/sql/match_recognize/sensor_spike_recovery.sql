-- Reproduzierbares Beispiel: Anstieg und anschliessende Stabilisierung pro Sensor
-- Hinweis: Pattern-Variablen sind in diesem Branch ein Zeichen lang.

CREATE OR REPLACE TEMP TABLE sensor_readings (
    sensor_id  VARCHAR,
    ts         TIMESTAMP,
    reading    DOUBLE
);

INSERT INTO sensor_readings VALUES
    ('S1', '2026-02-10 08:00:00', 19.0),
    ('S1', '2026-02-10 08:05:00', 21.0),
    ('S1', '2026-02-10 08:10:00', 24.0),
    ('S1', '2026-02-10 08:15:00', 23.0),
    ('S1', '2026-02-10 08:20:00', 22.0),
    ('S2', '2026-02-10 08:00:00', 30.0),
    ('S2', '2026-02-10 08:05:00', 31.0),
    ('S2', '2026-02-10 08:10:00', 33.0),
    ('S2', '2026-02-10 08:15:00', 32.0),
    ('S2', '2026-02-10 08:20:00', 31.0);

SELECT
    sensor_id,
    base_ts,
    peak_ts,
    settle_ts,
    avg_rise,
    final_reading
FROM sensor_readings
MATCH_RECOGNIZE (
    PARTITION BY sensor_id      
    ORDER BY ts
    MEASURES
        B.sensor_id      AS sensor_id,
        B.ts             AS base_ts,
        LAST(R.ts)       AS peak_ts,
        LAST(S.ts)       AS settle_ts,
        AVG(R.reading)   AS avg_rise,
        LAST(S.reading)  AS final_reading
    ONE ROW PER MATCH
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ('B R+ S+')
    DEFINE
        R AS reading > PREV(reading),
        S AS reading <= PREV(reading),
        B AS TRUE
);
