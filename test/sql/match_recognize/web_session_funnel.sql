-- Reproduzierbares Beispiel: einfache Funnel-Sequenz pro Web-Session
-- Hinweis: Pattern-Variablen sind in diesem Branch ein Zeichen lang.

CREATE OR REPLACE TEMP TABLE web_events (
    session_id  VARCHAR,
    event_ts    TIMESTAMP,
    event_type  VARCHAR,
    page        VARCHAR
);

INSERT INTO web_events VALUES
    ('sess_1', '2026-03-01 09:00:00', 'landing',  'home'),
    ('sess_1', '2026-03-01 09:01:00', 'browse',   'category'),
    ('sess_1', '2026-03-01 09:02:00', 'browse',   'product'),
    ('sess_1', '2026-03-01 09:03:00', 'cart',     'cart'),
    ('sess_1', '2026-03-01 09:04:00', 'checkout', 'checkout'),
    ('sess_2', '2026-03-01 10:00:00', 'landing',  'home'),
    ('sess_2', '2026-03-01 10:02:00', 'browse',   'search'),
    ('sess_2', '2026-03-01 10:03:00', 'cart',     'cart'),
    ('sess_2', '2026-03-01 10:05:00', 'checkout', 'checkout');

SELECT
    session_id,
    landing_ts,
    cart_ts,
    checkout_ts,
    browse_steps,
    checkout_page
FROM web_events
MATCH_RECOGNIZE (
    PARTITION BY session_id
    ORDER BY event_ts
    MEASURES
        L.session_id    AS session_id,
        L.event_ts      AS landing_ts,
        C.event_ts      AS cart_ts,
        H.event_ts      AS checkout_ts,
        COUNT(B.*)      AS browse_steps,
        LAST(H.page)    AS checkout_page
    ONE ROW PER MATCH
    AFTER MATCH SKIP TO NEXT ROW
    WITHIN 10 MINUTE
    PATTERN ('L B+ C H')
    DEFINE
        L AS event_type = 'landing',
        B AS event_type = 'browse',
        C AS event_type = 'cart',
        H AS event_type = 'checkout'
);