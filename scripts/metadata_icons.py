"""Project-owned 16-unit SVG geometry for Designer metadata types."""


def line(x1,y1,x2,y2):
    return f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}"/>'


def rect(x,y,w,h):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}"/>'


def circle(x,y,r):
    return f'<ellipse cx="{x}" cy="{y}" rx="{r}" ry="{r}"/>'


def poly(points):
    return f'<polyline points="{points}"/>'


def document(content):
    return poly('10,1.5 2.5,1.5 2.5,14.5 13.5,14.5 13.5,5 10,1.5 10,5 13.5,5')+content


def window(content):
    return rect(1.5,2,13,12)+line(1.5,5,14.5,5)+content


def grid(content):
    return rect(1.5,2,8.5,12)+line(1.5,5,10,5)+line(1.5,9.5,10,9.5)+line(5.5,5,5.5,14)+content


def database(content):
    return '<ellipse cx="6.5" cy="3.5" rx="4.8" ry="2"/>'+poly('1.7,3.5 1.7,11.8 4,13.5 9,13.5 11.3,11.8 11.3,3.5')+poly('1.7,7 4,8.5 8.5,8.5')+content


def globe(content=''):
    return circle(7.5,7.5,5.7)+'<ellipse cx="7.5" cy="7.5" rx="2.5" ry="5.7"/>'+line(2,7.5,13,7.5)+content


def folder(content):
    return poly('1.5,13.5 1.5,3 6,3 7.5,5 14.5,5 14.5,13.5 1.5,13.5')+content


GEOMETRY={
    'configuration': poly('8,1.5 14,5 14,11 8,14.5 2,11 2,5 8,1.5 8,8.5 14,5')+poly('2,5 8,8.5 8,14.5'),
    'common': line(4.5,4.5,11.5,11.5)+line(11.5,4.5,4.5,11.5)+circle(4,4,2)+circle(12,4,2)+circle(4,12,2)+circle(12,12,2),
    'subsystem': rect(6,1.5,4,3.5)+line(8,5,8,8)+line(3.5,8,12.5,8)+line(3.5,8,3.5,10.5)+line(12.5,8,12.5,10.5)+rect(1.5,10.5,4,3.5)+rect(10.5,10.5,4,3.5),
    'module': document(poly('6,7 4.5,9 6,11')+poly('10,7 11.5,9 10,11')+line(8.5,6.5,7.2,11.5)),
    'session-parameter': line(2,4,14,4)+line(2,8,14,8)+line(2,12,14,12)+rect(4,2.5,2,3)+rect(10,6.5,2,3)+rect(6,10.5,2,3),
    'role': poly('8,1.5 13.5,3.5 13,9 11.5,12 8,14.5 4.5,12 3,9 2.5,3.5 8,1.5')+circle(8,6.5,1.7)+line(8,8.2,8,11.5),
    'attribute': poly('1.5,7.5 7.5,1.5 13.5,1.5 14.5,7.5 8.5,13.5 1.5,7.5')+circle(11,4.5,1)+line(5.2,7,8.8,10.5),
    'tabular-section': rect(1.5,2,13,12)+line(1.5,5,14.5,5)+line(1.5,9.5,14.5,9.5)+line(6.5,2,6.5,14)+line(10.5,5,10.5,14),
    'exchange-plan': rect(1.5,2,4.5,5)+rect(10,9,4.5,5)+poly('8,3 13,3 13,6 11.5,4.5')+poly('8,13 3,13 3,10 4.5,11.5'),
    'selection-criterion': poly('1.5,2 14.5,2 9.5,7.5 9.5,13.5 6.5,12 6.5,7.5 1.5,2')+line(5,4.5,11,4.5),
    'event-subscription': document(poly('8.5,5.8 5.5,10 8.5,10 7,13 11,8.5 8,8.5 8.5,5.8')),
    'scheduled-job': circle(8,8,6)+poly('8,4 8,8 11,10')+line(6,1,10,1),
    'bot': rect(2.5,4.5,11,9)+line(8,1.5,8,4.5)+circle(8,1.5,.5)+circle(5.5,8,1)+circle(10.5,8,1)+line(6,11,10,11)+line(1,7,2.5,7)+line(13.5,7,15,7),
    'functional-option': rect(1.5,4,13,8)+circle(10.5,8,2.4)+line(4,8,5,8),
    'option-parameter': poly('2,5 2,2 14,2 14,5')+line(8,2,8,5)+rect(2,6,12,8)+line(4.5,10,11.5,10)+rect(7,8,2,4),
    'defined-type': poly('5,2 3,2 3,14 5,14')+poly('11,2 13,2 13,14 11,14')+line(6,5,10,5)+line(8,5,8,11),
    'settings-storage': database(circle(12,10,2.3)+line(12,6.5,12,7.7)+line(12,12.3,12,13.5)+line(8.5,10,9.7,10)+line(14.3,10,15.5,10)),
    'command': circle(8,8,6)+poly('6.5,4.5 11,8 6.5,11.5 6.5,4.5'),
    'command-group': folder(poly('7,7.5 10.8,10 7,12.5 7,7.5')),
    'form': window(rect(3.5,7,3,2)+line(8.5,8,12.5,8)+rect(3.5,10.5,3,2)+line(8.5,11.5,12.5,11.5)),
    'template': document(line(2.7,7,13.3,7)+line(2.7,10.5,13.3,10.5)+line(7,7,7,14)),
    'image': rect(1.5,2,13,12)+circle(10.5,5.5,1.5)+poly('2,12 6,7.5 9,10.5 11,9 14,12'),
    'xdto': poly('4,2 1.5,4.5 4,7')+poly('12,2 14.5,4.5 12,7')+rect(6,2.5,4,4)+line(8,6.5,8,9)+line(4,9,12,9)+line(4,9,4,11)+line(12,9,12,11)+rect(2,11,4,3)+rect(10,11,4,3),
    'web-service': globe()+poly('10.5,12 13.5,12 13.5,15')+line(12,13.5,15,13.5),
    'http-service': circle(8,8,6)+line(5.5,4.5,5.5,11.5)+line(10.5,4.5,10.5,11.5)+line(5.5,8,10.5,8),
    'ws-reference': '<ellipse cx="5.5" cy="5.5" rx="3" ry="3"/><ellipse cx="10.5" cy="10.5" rx="3" ry="3"/>'+line(5.5,10.5,10.5,5.5),
    'websocket': poly('2,4 12.5,4 10,1.5')+line(12.5,4,10,6.5)+poly('14,12 3.5,12 6,9.5')+line(3.5,12,6,14.5)+line(2,7,2,9)+line(14,7,14,9),
    'integration': poly('5,1.5 5,5 11,5 11,1.5')+poly('3,5 13,5 12,9.5 8,12 4,9.5 3,5')+line(8,12,8,15),
    'style-element': circle(4,4,2)+rect(10,2,4,4)+poly('4,10 6,14 2,14 4,10')+line(10,10,14,14)+line(14,10,10,14),
    'style': poly('9.5,1.5 14.5,6.5 8,13 3,8 9.5,1.5')+line(6,5,11,10)+poly('3,8 1.5,12 1.5,14.5 5.5,14.5 8,13'),
    'language': globe()+poly('10.5,14.5 12.5,9.5 14.5,14.5')+line(11.2,13,13.8,13),
    'constant': rect(2,2,12,12)+line(4.5,6.5,11.5,6.5)+line(4.5,9.5,11.5,9.5),
    'catalog': rect(2,1.5,12,13)+line(5,1.5,5,14.5)+line(8,5,11.5,5)+line(8,8,11.5,8)+line(8,11,10.5,11),
    'document': document(line(5,7.5,11,7.5)+line(5,10,11,10)+line(5,12,9,12)),
    'document-numerator': document(line(6,6.5,5,12)+line(10,6.5,9,12)+line(4.5,8.2,11,8.2)+line(4,10.5,10.5,10.5)),
    'document-sequence': rect(1,2,3,3)+rect(6.5,6.5,3,3)+rect(12,11,3,3)+poly('5,3.5 9,3.5 9,5')+poly('7,12.5 3,12.5 3,7')+poly('3,7 1.5,8.5')+line(3,7,4.5,8.5)+poly('11,8 13.5,8 13.5,10'),
    'document-journal': rect(2,2,12,12)+line(5,2,5,14)+line(7,5,12,5)+line(7,8,12,8)+line(7,11,10.5,11),
    'enumeration': rect(2,2,2,2)+rect(2,7,2,2)+rect(2,12,2,2)+line(6.5,3,14,3)+line(6.5,8,12,8)+line(6.5,13,13,13),
    'report': document(line(5,12,5,9)+line(8,12,8,6.5)+line(11,12,11,8)),
    'processor': poly('5,1.5 2,1.5 2,14.5 5,14.5')+poly('11,1.5 14,1.5 14,14.5 11,14.5')+poly('6.5,5 10.5,8 6.5,11 6.5,5'),
    'characteristic-plan': grid(poly('12.5,6.5 15,9 12.5,11.5 10,9 12.5,6.5')),
    'account-plan': grid(line(11,7,15,7)+line(13,7,13,13)),
    'calculation-plan': grid(poly('15,6.5 11.5,6.5 13.5,9.5 11.5,13 15,13')),
    'information-register': grid(circle(13,7,0.5)+line(13,9,13,13)),
    'accumulation-register': grid(line(13,8,13,13)+line(10.5,10.5,15.5,10.5)),
    'accounting-register': grid(line(11,9,15,9)+line(11,12,15,12)),
    'calculation-register': grid(poly('15,6.5 11.5,6.5 13.5,9.5 11.5,13 15,13'))+line(2,1,9.5,1),
    'business-process': rect(1.5,1.5,4,4)+rect(10.5,10.5,4,4)+poly('7.5,3.5 12.5,3.5 12.5,8 10.8,6.5')+poly('8.5,12.5 3.5,12.5 3.5,8 5.2,9.5'),
    'task': rect(2,1.5,12,13)+poly('4.5,7.5 7,10 11.5,5.5')+line(5,12,11,12),
    'external-data': database(poly('10.5,8 14.5,8 14.5,13 12,13')+poly('12.5,6 14.5,8 12.5,10')),
}

ICON_ROLES={
    **{name:'blue' for name in GEOMETRY},
    **{name:'yellow' for name in ('configuration','common','subsystem','catalog','command-group','selection-criterion','event-subscription','report')},
    **{name:'green' for name in ('functional-option','option-parameter','command','image','business-process','task')},
    **{name:'teal' for name in ('exchange-plan','websocket','integration','information-register','accumulation-register','accounting-register','calculation-register','external-data')},
    **{name:'purple' for name in ('role','defined-type','enumeration','characteristic-plan','account-plan','calculation-plan','xdto','constant')},
    **{name:'pink' for name in ('style-element','style')},
}
GEOMETRY={'meta-'+name:geometry for name,geometry in GEOMETRY.items()}
ICON_ROLES={'meta-'+name:role for name,role in ICON_ROLES.items()}
