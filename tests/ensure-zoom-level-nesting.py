import sys
import json

data = json.load(sys.stdin)

layers_per_zoom = {}
reported = set()
maxzoom = 0

for tile in data['features']:
    zoom = tile['properties']['zoom']
    if zoom > maxzoom:
        maxzoom = zoom

    for layer in tile['features']:
        layername = layer['properties']['layer']

        for feature in layer['features']:
            if zoom not in layers_per_zoom:
                layers_per_zoom[zoom] = {}
            if layername not in layers_per_zoom[zoom]:
                layers_per_zoom[zoom][layername] = set()

            if 'tippecanoe:retain_points_multiplier_sequence' in feature['properties']:
                del(feature['properties']['tippecanoe:retain_points_multiplier_sequence'])
            if 'tippecanoe:retain_points_multiplier_first' in feature['properties']:
                del(feature['properties']['tippecanoe:retain_points_multiplier_first'])

            properties = json.dumps(feature['properties'])
            layers_per_zoom[zoom][layername].add(properties)

for zoom in range(maxzoom + 1):
    if zoom in layers_per_zoom:
        for layer in layers_per_zoom[zoom]:
            for feature in layers_per_zoom[zoom][layer]:
                if feature not in reported:
                    for z in range(zoom + 1, maxzoom + 1):
                        if feature not in layers_per_zoom[z][layer]:
                            print("in", zoom, "but not in", z, ":", feature)
                            reported.add(feature)
