"""Project-owned thin-line glyphs for Designer context and configuration menus."""
from metadata_icons import line, rect, circle, poly, document, window


def plus(x,y):
    return line(x-2,y,x+2,y)+line(x,y-2,x,y+2)


def magnifier(content):
    return circle(5.7,9.5,3.8)+line(8.5,12.3,11,14.8)+content


def storage(content):
    return '<ellipse cx="6" cy="3.5" rx="4.5" ry="2"/>'+poly('1.5,3.5 1.5,12 4,14 8,14')+line(10.5,3.5,10.5,6)+content


GEOMETRY={
    'module-open': window(poly('5,7 3.5,9 5,11')+poly('9,7 10.5,9 9,11'))+poly('10.5,11 14.5,11 12.5,9 14.5,11 12.5,13'),
    'predefined-data': document(rect(5,6,2,2)+line(9,7,11,7)+rect(5,10,2,2)+line(9,11,11,11)),
    'extension-add': poly('2,14 2,4 6,4 6,2 9,2 9,4 13,4 13,8 15,8 15,11 13,11 13,14 2,14')+plus(7,9),
    'standard-attributes': poly('1.5,7.5 7.5,1.5 14,1.5 14,8 8,14.5 1.5,7.5')+circle(11,4.5,1)+line(5,8,9,8)+line(6,10.5,9,10.5),
    'characteristics': poly('8,1.5 14.5,8 8,14.5 1.5,8 8,1.5')+line(5,8,11,8)+line(8,5,8,11),
    'indexes': rect(1.5,2,8.5,12)+line(1.5,5,10,5)+line(5,5,5,14)+circle(11,9,2)+poly('11,11 11,14.5 14,14.5 14,13'),
    'help': poly('8,3 4.5,1.8 1.5,3 1.5,14 4.5,12.8 8,14 8,3 11.5,1.8 14.5,3 14.5,14 11.5,12.8 8,14')+line(10,5.5,12.5,5.5)+line(10,8,12.5,8)+line(4,5.5,6,5.5)+line(4,8,6,8),
    'references-to': magnifier(poly('8,3 14,3 14,7')+poly('12,5 14,7 15.2,5')),
    'references-in': magnifier(poly('14.5,7 14.5,3 8,3')+poly('10,1 8,3 10,5')),
    'advanced': window(line(4,8,12,8)+line(4,11.5,12,11.5)+rect(6,6.5,2,3)+rect(9,10,2,3)),
    'properties': document(line(5,7,11,7)+line(5,10,11,10)+line(5,12.5,8,12.5)),
    'derive-builder': rect(1.5,1.5,5,5)+rect(9.5,9.5,5,5)+poly('9,3.5 12,3.5 12,7.5 10,5.5')+line(12,7.5,14,5.5),
    'movement-builder': rect(1.5,2,8,12)+line(1.5,6,9.5,6)+line(1.5,10,9.5,10)+line(5,6,5,14)+poly('11,4 14.5,8 11,12')+line(10,8,14.5,8),
    'print-builder': poly('4,5 4,1.5 12,1.5 12,5')+rect(1.5,5,13,6)+rect(4,9,8,5.5)+plus(8,11.5),
    'configuration-compare': rect(1.5,1.5,4,5)+rect(10.5,9.5,4,5)+poly('9,3 13,3 13,6 11.5,4.5')+poly('7,13 3,13 3,10 4.5,11.5'),
    'configuration-check': line(1.5,3.5,5.5,3.5)+poly('9,2 11,4 14.5,1')+line(1.5,8.5,5.5,8.5)+poly('9,7 11,9 14.5,6')+line(1.5,13,5.5,13),
    'package-check': poly('8,1.5 14,5 14,11 8,14.5 2,11 2,5 8,1.5 8,8.5 14,5')+poly('2,5 8,8.5 8,14.5')+poly('9,11 11,13 15,8.5'),
    'database-compare': storage(circle(11,10,3)+line(13.2,12.2,15.2,14.2)),
    'database-save': storage(rect(8,7,7,7)+rect(10,7,3,2)+rect(10,11,3,3)),
    'database-return': storage(poly('11,7 8,10 11,13')+poly('8,10 13.5,10 14.5,11.5 14.5,14')),
    'support-update': rect(2,2,8.5,12)+line(4,5,8.5,5)+poly('10.5,8 14.5,8 12.5,6 14.5,8 12.5,10')+line(5,8,8,8)+line(5,11,8,11),
    'support-settings': rect(1.5,1.5,9,13)+line(4,5,8,5)+circle(11.5,10.5,2.5)+line(11.5,6.5,11.5,8)+line(11.5,13,11.5,15)+line(7.5,10.5,9,10.5)+line(14,10.5,15.5,10.5),
    'repository-connect': storage(poly('10,7 10,9 14,9 14,7')+poly('9,9 9,12 11,13.5 13,13.5 15,12 15,9')+line(12,13.5,12,15)),
    'repository-disconnect': storage(line(9,8,15,14)+line(15,8,9,14)),
    'repository-open': storage(poly('8,9 8,6.5 11,6.5 12.5,8 15,8 14,14 8,14 8,9')),
    'repository-close': storage(rect(8,7,7,7)+line(10,9,13,12)+line(13,9,10,12)),
    'repository-create': storage(plus(11.5,10.5)),
    'delivery-build': poly('2,2 10,2 10,5 14,5 14,14 2,14 2,2')+line(2,6,10,6)+poly('6,9 9,9 9,12')+line(9,9,6,12),
    'delivery-settings': rect(1.5,1.5,8,13)+line(1.5,5,9.5,5)+circle(11.5,10.5,2.5)+line(11.5,6.5,11.5,8)+line(11.5,13,11.5,15)+line(7.5,10.5,9,10.5)+line(14,10.5,15.5,10.5),
}
ICON_ROLES={name:'blue' for name in GEOMETRY}
ICON_ROLES.update({'extension-add':'green','standard-attributes':'green','characteristics':'purple','indexes':'teal','help':'green','references-to':'teal','references-in':'teal','derive-builder':'teal','movement-builder':'teal'})
ICON_ROLES.update({name:'teal' for name in ('database-compare','database-save','database-return','repository-connect','repository-disconnect','repository-open','repository-close','repository-create')})
ICON_ROLES.update({'configuration-check':'green','package-check':'green','configuration-compare':'purple','support-update':'yellow','support-settings':'yellow'})
ICON_ROLES.update({'delivery-build':'blue','delivery-settings':'blue'})
GEOMETRY={'menu-'+name:geometry for name,geometry in GEOMETRY.items()}
ICON_ROLES={'menu-'+name:role for name,role in ICON_ROLES.items()}
