"""Own 16-unit thin-line icons for module editing and the remaining main menus."""
from metadata_icons import line, rect, circle, poly, document, window, globe, folder


def plus(x,y):
    return line(x-2,y,x+2,y)+line(x,y-2,x,y+2)


def cross(x,y):
    return line(x-1.8,y-1.8,x+1.8,y+1.8)+line(x+1.8,y-1.8,x-1.8,y+1.8)


def bookmark(content=''):
    return poly('3,1.5 10,1.5 10,12.5 6.5,10 3,12.5 3,1.5')+content


def arrow_up(x,y):
    return poly(f'{x-2},{y+2} {x},{y} {x+2},{y+2}')+line(x,y,x,y+5)


def arrow_down(x,y):
    return poly(f'{x-2},{y+3} {x},{y+5} {x+2},{y+3}')+line(x,y,x,y+5)


def point(content=''):
    return poly('4,1.5 8.5,1.5 11,4 11,8.5 8.5,11 4,11 1.5,8.5 1.5,4 4,1.5')+content


def lines():
    return line(7.5,3,14.5,3)+line(7.5,6.5,12.5,6.5)+line(7.5,10,14.5,10)+line(7.5,13.5,12.5,13.5)


def person(content=''):
    return circle(6,4.5,2.5)+poly('1.5,14 2,10 4,8.5 8,8.5 10,10 10.5,14')+content


GEOMETRY={
    'file-close': document(cross(8,10)),
    'page-settings': rect(3.5,2,9,12)+line(1,5,6,5)+line(1,11,6,11)+line(8,0.8,8,4)+line(8,12,8,15.2),
    'replace': poly('2,6 4,1.5 6,6')+line(3,4.5,5,4.5)+rect(10,9,4,5)+line(10,11.5,14,11.5)+poly('8,3 13,3 13,6 11.5,4.5')+poly('8,13 3,13 3,10 4.5,11.5'),
    'bookmark': bookmark(),
    'bookmark-next': bookmark(arrow_down(13,8)),
    'bookmark-previous': bookmark(arrow_up(13,7)),
    'bookmark-clear': bookmark(cross(12,12)),
    'translate-ui': globe(poly('9,14 11.5,8 14,14')+line(10,12,13,12)),
    'find-tree': rect(1.5,1.5,4,3)+line(3.5,4.5,3.5,12)+line(3.5,8,7,8)+line(3.5,12,7,12)+circle(11,9,3)+line(13,11.5,15,14),
    'fold-all': rect(1.5,2,5,5)+line(3,4.5,5,4.5)+rect(8.5,9,5,5)+line(10,11.5,12,11.5)+poly('4,8.5 4,12 7,12'),
    'unfold-all': rect(1.5,2,5,5)+line(3,4.5,5,4.5)+line(4,3.5,4,5.5)+rect(8.5,9,5,5)+line(10,11.5,12,11.5)+line(11,10.5,11,12.5)+poly('4,8.5 4,12 7,12'),
    'fold': rect(2,2,12,12)+line(5,8,11,8),
    'unfold': rect(2,2,12,12)+plus(8,8),
    'comment-add': line(3,3,1.5,10)+line(7,3,5.5,10)+plus(12,10.5),
    'comment-remove': line(3,3,1.5,10)+line(7,3,5.5,10)+line(9.5,10.5,14.5,10.5),
    'linebreak-add': poly('4,2 8,2 8,7.5 2,7.5 4,5.5')+line(2,7.5,4,9.5)+plus(12,11),
    'linebreak-remove': poly('4,2 8,2 8,7.5 2,7.5 4,5.5')+line(2,7.5,4,9.5)+line(9.5,11,14.5,11),
    'module-check': document(poly('5,10 7,12 11.5,7.5')),
    'procedures': poly('10,1.5 8,1.5 6.5,3 6.5,13 5,14.5 3.5,14.5')+line(4,6,10,6)+line(12,4,14,4)+line(12,8,14,8)+line(11,12,14,12),
    'query-builder': rect(1.5,2,10,12)+line(1.5,5.5,11.5,5.5)+line(5,5.5,5,14)+poly('8,11 13,6 15,8 10,13 7,14 8,11'),
    'query-process': rect(1.5,2,8,12)+line(1.5,6,9.5,6)+line(5,6,5,14)+poly('11,8 15,11 11,14 11,8'),
    'format': line(2,3,14,3)+line(2,6.5,11,6.5)+line(2,10,14,10)+line(2,13.5,11,13.5),
    'indent': lines()+poly('1.5,5.5 4.5,8 1.5,10.5')+line(1.5,8,4.5,8),
    'outdent': lines()+poly('4.5,5.5 1.5,8 4.5,10.5')+line(1.5,8,4.5,8),
    'goto-line': document(line(6,6.5,11,6.5)+line(6,10,11,10))+poly('1,8 4,10 1,12')+line(1,10,4,10),
    'breakpoint': point(),
    'breakpoint-settings': point(line(12.5,7,12.5,14)+line(9,10.5,15,10.5)+circle(12.5,10.5,1.5)),
    'breakpoint-toggle': point(poly('8.5,10 11,13 15,8')),
    'breakpoint-clear': point(cross(12,12)),
    'breakpoint-up': point(arrow_up(13,8)),
    'breakpoint-down': point(arrow_down(13,8)),
    'breakpoint-cut': point(circle(10,13,1.5)+circle(14,13,1.5)+line(11,11.8,14,7)+line(13,11.8,10,7)),
    'breakpoint-copy': point(rect(9,8,5,6)+line(11,10,15.5,10)+line(15.5,10,15.5,15.5)),
    'breakpoint-paste': point(rect(9,8,6,7)+rect(10.5,7,3,2)),
    'performance': circle(7,9,5.5)+line(5,1.5,9,1.5)+line(7,1.5,7,3.5)+poly('7,5.5 7,9 10,10')+line(11,3,13,1.5),
    'extensions': poly('2,14 2,4 6,4 6,2 9,2 9,4 13,4 13,8 15,8 15,11 13,11 13,14 2,14'),
    'compare-configurations': document(line(5,7,8,7))+poly('9,10 14,10 12,8 14,10 12,12')+poly('7,13 2,13 4,11 2,13 4,15'),
    'client-thin': window(poly('6,7 10,9.5 6,12 6,7')),
    'client-web': globe(poly('6.5,5 11,8 6.5,11 6.5,5')),
    'client-mobile': rect(3.5,1.5,9,13)+line(6.5,12,9.5,12)+poly('6,4.5 10,7.5 6,10.5 6,4.5'),
    'client-mobile-app': rect(2.5,1.5,10,13)+line(6,12,9,12)+poly('8.5,5 14,8.5 8.5,12 8.5,5'),
    'client-thick': window(rect(3.5,7,3,5)+poly('9,7 13,9.5 9,12 9,7')),
    'users': person(circle(12.5,6,1.8)+poly('11.5,9 14,9 15,11 15,14')),
    'users-active': person(circle(12.5,11,2.5)+poly('12.5,9.5 12.5,11 14,12')),
    'event-log': rect(1.5,3,13,11.5)+line(1.5,6,14.5,6)+line(4.5,3,4.5,14.5)+line(7,9,12,9)+line(7,12,10,12),
    'auth-lock': person(rect(9,9,6,5)+poly('10.5,9 10.5,7 13.5,7 13.5,9')),
    'record': circle(8,8,6)+circle(8,8,2.5),
    'calculator': rect(3,1.5,10,13)+rect(5,3.5,6,3)+line(5,9,6,9)+line(9,9,10,9)+line(5,12,6,12)+line(9,12,10,12),
    'calendar': rect(1.5,3,13,11.5)+line(1.5,6.5,14.5,6.5)+line(5,1.5,5,5)+line(11,1.5,11,5)+line(5,9,6,9)+line(10,9,11,9)+line(5,12,6,12)+line(10,12,11,12),
    'session-lock': rect(3,7,10,7.5)+poly('5,7 5,3.5 6.5,1.5 9.5,1.5 11,3.5 11,7')+circle(8,10,1)+line(8,11,8,12.5),
    'settings': poly('10,1.5 7,4.5 7.5,7 2,12.5 3.5,14 9,8.5 11.5,9 14.5,6 11,6 10,5 10,1.5'),
    'window-new': window(plus(8,9.5)),
    'split-horizontal': window(line(1.5,9.5,14.5,9.5)),
    'split-vertical': window(line(8,5,8,14)),
    'navigate-back': window(poly('8,7 5,10 8,13')+line(5,10,12,10)),
    'navigate-forward': window(poly('8,7 11,10 8,13')+line(4,10,11,10)),
    'messages': window(line(4,7.5,12,7.5)+line(4,10,10,10)+line(4,12.5,11,12.5)),
    'help-question': circle(8,8,6)+poly('6,5.5 6.5,4 9,4 10.5,5.5 10,7 8,8.5 8,9.5')+line(8,11.8,8,12),
}
ICON_ROLES={name:'blue' for name in GEOMETRY}
ICON_ROLES.update({name:'ink' for name in ('fold','unfold','fold-all','unfold-all','format','indent','outdent','page-settings','file-close','window-new','split-horizontal','split-vertical','navigate-back','navigate-forward','settings')})
ICON_ROLES.update({name:'red' for name in GEOMETRY if name.startswith('breakpoint') or name in ('record','comment-remove','linebreak-remove')})
ICON_ROLES.update({name:'green' for name in ('comment-add','linebreak-add','module-check','client-thin','client-thick')})
ICON_ROLES.update({name:'teal' for name in ('query-builder','query-process','client-web','event-log','users-active')})
ICON_ROLES.update({name:'purple' for name in ('bookmark','bookmark-next','bookmark-previous','bookmark-clear','procedures','extensions','translate-ui')})
ICON_ROLES.update({name:'yellow' for name in ('calculator','calendar','session-lock','auth-lock','help-question','performance')})
GEOMETRY={'editor-'+name:geometry for name,geometry in GEOMETRY.items()}
ICON_ROLES={'editor-'+name:role for name,role in ICON_ROLES.items()}
