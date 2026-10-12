#!/usr/bin/env python3
"""Check OOXML links and normalize known PptxGenJS 4.0.1 package defects.

Writes a separate file. Never repairs missing referenced objects or alters slide content.
"""
import argparse
from io import BytesIO
from pathlib import Path
import posixpath
import re
import xml.etree.ElementTree as ET
from zipfile import ZipFile, ZIP_DEFLATED

CT = 'http://schemas.openxmlformats.org/package/2006/content-types'
C = 'http://schemas.openxmlformats.org/drawingml/2006/chart'
ET.register_namespace('', CT)
ET.register_namespace('c', C)
ET.register_namespace('a', 'http://schemas.openxmlformats.org/drawingml/2006/main')
ET.register_namespace('r', 'http://schemas.openxmlformats.org/officeDocument/2006/relationships')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('output already exists; choose a new filename')
    with ZipFile(args.source) as archive:
        names = set(archive.namelist())
        referenced = set()
        for name in names:
            if not name.endswith('.rels'):
                continue
            base = posixpath.dirname(posixpath.dirname(name))
            for relation in ET.fromstring(archive.read(name)):
                if relation.get('TargetMode') == 'External':
                    continue
                target = relation.get('Target', '')
                resolved = target.lstrip('/') if target.startswith('/') else posixpath.normpath(posixpath.join(base, target))
                if resolved not in names:
                    raise ValueError(f'missing referenced object: {resolved}')
                referenced.add(resolved)
        types = ET.fromstring(archive.read('[Content_Types].xml'))
        removed = []
        for item in list(types):
            if item.tag != f'{{{CT}}}Override':
                continue
            name = item.get('PartName', '').lstrip('/')
            if name in names:
                continue
            if name in referenced or not re.fullmatch(r'ppt/slideMasters/slideMaster\d+\.xml', name):
                raise ValueError(f'missing declared object: {name}')
            types.remove(item)
            removed.append(name)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        normalized = 0
        removed_axes = 0
        fixed_tables = 0
        with args.output.open('xb') as handle, ZipFile(handle, 'w', ZIP_DEFLATED) as output:
            for info in archive.infolist():
                content = ET.tostring(types, encoding='utf-8', xml_declaration=True) if info.filename == '[Content_Types].xml' else archive.read(info)
                # PptxGenJS 4.0.1 issue #1531: category charts have a stray quote
                # in their embedded workbook table range. Keep all cells unchanged.
                if re.fullmatch(r'ppt/embeddings/[^/]+\.xlsx', info.filename):
                    buffer = BytesIO()
                    with ZipFile(BytesIO(content)) as workbook, ZipFile(buffer, 'w', ZIP_DEFLATED) as fixed:
                        for part in workbook.infolist():
                            data = workbook.read(part)
                            if re.fullmatch(r'xl/tables/table\d+\.xml', part.filename):
                                table = ET.fromstring(data)
                                span = table.get('ref', '')
                                valid = r'\$?[A-Z]+\$?\d+(?::\$?[A-Z]+\$?\d+)?'
                                if span.endswith("'") and re.fullmatch(valid, span[:-1]):
                                    data = data.replace(('ref="' + span + '"').encode(), ('ref="' + span[:-1] + '"').encode(), 1)
                                    fixed_tables += 1
                                elif not re.fullmatch(valid, span):
                                    raise ValueError('invalid embedded workbook table range')
                            fixed.writestr(part, data)
                    content = buffer.getvalue()
                # PptxGenJS writes multi-level references even for one category level.
                # Express that single level as the equivalent standard string cache.
                if re.fullmatch(r'ppt/charts/chart\d+\.xml', info.filename):
                    chart = ET.fromstring(content)
                    for plot in chart.findall(f'.//{{{C}}}plotArea'):
                        axes = {axis.get('val') for axis in plot.findall(f'./{{{C}}}catAx/{{{C}}}axId') + plot.findall(f'./{{{C}}}valAx/{{{C}}}axId')}
                        for bars in plot.findall(f'{{{C}}}barChart'):
                            ids = bars.findall(f'{{{C}}}axId')
                            if len(axes) == 2 and len(ids) == 3 and sum(axis.get('val') in axes for axis in ids) == 2:
                                for axis in ids:
                                    if axis.get('val') not in axes:
                                        bars.remove(axis)
                                        removed_axes += 1
                    for parent in chart.iter():
                        for reference in list(parent):
                            if reference.tag != f'{{{C}}}multiLvlStrRef':
                                continue
                            cache = reference.find(f'{{{C}}}multiLvlStrCache')
                            levels = [] if cache is None else cache.findall(f'{{{C}}}lvl')
                            if len(levels) != 1:
                                continue
                            reference.tag = f'{{{C}}}strRef'
                            cache.tag = f'{{{C}}}strCache'
                            for point in levels[0]:
                                cache.append(point)
                            cache.remove(levels[0])
                            normalized += 1
                    content = ET.tostring(chart, encoding='utf-8', xml_declaration=True)
                output.writestr(info, content)
        print(f'Referenced objects intact; removed {len(removed)} unused declarations')
        print(f'Normalized {normalized} single-level chart references without changing data')
        print(f'Removed {removed_axes} nonexistent series-axis references from 2D bars')
        print(f'Fixed {fixed_tables} embedded workbook table ranges; cells unchanged')
        print(args.output.resolve())

if __name__ == '__main__':
    main()
