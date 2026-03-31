CREATE TABLE crimes (
    id            INTEGER PRIMARY KEY,
    datetime      TIMESTAMP,
    primary_type  VARCHAR(50),
    lat           DECIMAL(9, 6),
    lon           DECIMAL(9, 6)
);

INSERT INTO crimes (id, datetime, primary_type, lat, lon) VALUES
(1, '2018-01-02 05:30:00', 'ASSAULT', 41.69, -87.66),
(2, '2018-01-02 05:35:00', 'ROBBERY', 41.10, -87.50),
(3, '2018-01-02 05:40:00', 'BURGLARY', 41.34, -87.57),
(4, '2018-01-02 05:45:00', 'ROBBERY', 41.13, -87.55),
(5, '2018-01-02 05:50:00', 'ASSAULT', 41.25, -87.61),
(6, '2018-01-02 05:55:00', 'BATTERY', 41.12, -87.51),
(7, '2018-01-02 06:00:00', 'NARCOTICS', 41.17, -87.59),
(8, '2018-01-02 06:05:00', 'MOTOR VEHICLE THEFT', 41.11, -87.53);

SELECT * FROM crimes MATCH_RECOGNIZE (
    ORDER BY datetime
    MEASURES R.id AS RID, B.id AS BID, M.id AS MID
    ONE ROW PER MATCH
    AFTER MATCH SKIP TO NEXT ROW
    WITHIN '30 minutes'
    PATTERN ('R Z* B Z* M')
    DEFINE Z as true,
    R AS R.primary_type = 'ROBBERY',
    B AS B.primary_type = 'BATTERY'
    AND B. lon BETWEEN R.lon - 0.05 AND R.lon + 0.05
    AND B.lat BETWEEN R.lat - 0.02 AND R.lat + 0.02,
    M AS M.primary_type = 'MOTOR VEHICLE THEFT'
    AND M. lon BETWEEN R.lon - 0.05 AND R.lon + 0.05
    AND M.lat BETWEEN R.lat - 0.02 AND R.lat + 0.02
);