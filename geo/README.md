# Country borders

Which country Home Assistant's home is in, for `src/hassmic/geo.c`. Home Assistant tells devices its time zone only as
a rule (offset and summer time), which many countries share; the country of its home location (`zone.home`'s latitude
and longitude) says which zone of that rule it is, and so the language of "Language you speak" on Automatic. Only the
country found goes into the Echo's state; the coordinates are never stored or logged.

`borders.bin` (zlib'd, 363 kB, built into hassmic) holds every country's outer rings, and the holes that are an
enclave's outline (for what is near only), cut down to what that lookup needs: land borders (enclaves' included, and
coasts within ~20 km of another country) to ~500 m, small enclaves to ~50 m, other coasts to ~10 km, islands under
~25 km² with no land border left out but each country's largest, points in 0.0005°. A house off a coarse coast takes the nearest country
within 50 km; within 3 km of a border the other country is named too, and the Wi-Fi's country decides there. Its format
and the same lookup in Python: `tools/geo/geo.py`.

Made by `tools/geo/build.py <ne_10m_admin_0_countries.geojson> geo/borders.bin` from Natural Earth v5.1.2 (the URL is
in the script). Its self-check against the full borders, at the time of building: 1500 random points in Europe all
right; of 1500 points 0.5-6 km from a land border anywhere (enclaves' included), 1481 right and 19 in the neighbour
with the right country named as near; none wrong otherwise. `make unit-geo` holds geo.c to geo.py (half-way
coordinates included) and checks 66 places: exactly, but border towns, which may come out as the neighbour named near.
Natural Earth at 1:10m lacks the smallest exclaves (Büsingen, Campione d'Italia): they come out as the country around
them, with theirs named near.

## Source and licence

[Natural Earth](https://www.naturalearthdata.com/), Admin 0 – Countries, 1:10m, v5.1.2: public domain
([terms](https://www.naturalearthdata.com/about/terms-of-use/)). Codes are its `ISO_A2_EH`; the Sovereign Base Areas,
Northern Cyprus and the buffer zone count as CY, Somaliland as SO, Guantánamo as CU; disputed reefs and unclaimed land
are left out. Natural Earth draws de facto borders; they decide a time zone's name here, nothing more.
