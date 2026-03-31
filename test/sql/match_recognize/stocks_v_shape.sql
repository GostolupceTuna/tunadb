-- Reproduzierbares Beispiel: V-foermige Kursbewegung pro Symbol
-- Hinweis: Pattern-Variablen sind in diesem Branch ein Zeichen lang.

CREATE OR REPLACE TEMP TABLE stock_ticks (
    symbol  VARCHAR,
    ts      TIMESTAMP,
    price   DOUBLE
);

INSERT INTO stock_ticks VALUES
    ('AAA', '2026-01-01 09:00:00', 100.0),
    ('AAA', '2026-01-01 10:00:00',  95.0),
    ('AAA', '2026-01-01 11:00:00',  90.0),
    ('AAA', '2026-01-01 12:00:00',  96.0),
    ('AAA', '2026-01-01 13:00:00', 102.0),
    ('BBB', '2026-01-01 09:00:00',  55.0),
    ('BBB', '2026-01-01 10:00:00',  48.0),
    ('BBB', '2026-01-01 11:00:00',  44.0),
    ('BBB', '2026-01-01 12:00:00',  47.0),
    ('BBB', '2026-01-01 13:00:00',  51.0);

SELECT
    symbol,
    start_ts,
    bottom_ts,
    rebound_ts,
    start_price,
    bottom_price,
    rebound_price
FROM stock_ticks
MATCH_RECOGNIZE (
    PARTITION BY symbol
    ORDER BY ts
    MEASURES
        S.symbol     AS symbol,
        S.ts         AS start_ts,
        LAST(D.ts)   AS bottom_ts,
        LAST(U.ts)   AS rebound_ts,
        S.price      AS start_price,
        LAST(D.price) AS bottom_price,
        LAST(U.price) AS rebound_price
    ONE ROW PER MATCH
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ('S D+ U+')
    DEFINE
        D AS price < PREV(price),
        U AS price > PREV(price),
        S AS TRUE
);