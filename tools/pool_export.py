#!/usr/bin/env python3
"""PoolEdit XML to the parser's input, and a C++ header of the object IDs.

PoolEdit (github.com/moehman/PoolEditDist) saves pools with objects referenced by name.
Its parser (github.com/moehman/PoolEditParser) needs the "embedded" export, where every
object and every include_object carries a numeric id. PoolEdit makes that export from its
menu; this does the same step from the command line for the pools here, which give every
object an explicit id: it copies each object's id onto the include_objects that name it,
fills in output string lengths, and writes the IDs as constants for the terminal client,
in a namespace named after the header (spreader_ids.hpp: spreader_pool).

Usage: pool_export.py <pool.xml> <export.xml> <ids.hpp>
"""
import sys
import xml.etree.ElementTree as ET

NOT_OBJECTS = {'objectpool', 'include_object'}


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip().splitlines()[-1])
    source, export, header = sys.argv[1:]
    tree = ET.parse(source)
    root = tree.getroot()

    ids = {}
    for e in root.iter():
        if e.tag in NOT_OBJECTS or e.tag.startswith('command_'):
            continue
        name, oid = e.get('name'), e.get('id', '')
        if not name:
            sys.exit(f'{source}: a <{e.tag}> has no name')
        if not oid.isdigit():
            sys.exit(f'{source}: {name} has no numeric id; every object here needs one')
        if name in ids:
            sys.exit(f'{source}: the name {name} is used twice')
        if int(oid) in ids.values():
            sys.exit(f'{source}: id {oid} is used twice')
        ids[name] = int(oid)
        if e.tag == 'outputstring' and not e.get('length'):
            e.set('length', str(len(e.get('value', ''))))

    for inc in root.iter('include_object'):
        name = inc.get('name')
        if name not in ids:
            sys.exit(f'{source}: include_object names {name}, which is not in the pool')
        inc.set('id', str(ids[name]))

    tree.write(export, encoding='UTF-8', xml_declaration=True)

    base = header.split('/')[-1].rsplit('.', 1)[0]
    space = base[:-4] + '_pool' if base.endswith('_ids') else base
    with open(header, 'w') as out:
        out.write(f'// Object IDs of {source.split("/")[-1]}, written by tools/pool_export.py: do not edit.\n')
        out.write('#pragma once\n\n#include <cstdint>\n\n')
        out.write(f'namespace {space}\n{{\n')
        for name, oid in sorted(ids.items(), key=lambda kv: kv[1]):
            out.write(f'\tconstexpr std::uint16_t {name} = {oid};\n')
        out.write('}\n')


if __name__ == '__main__':
    main()
