'use strict';

// Values match SyntaxRole in protocol.h.
function classify(scopes) {
    const query = scopes.some(scope => scope === 'source.sdbl' || scope.endsWith('.sdbl'));
    for (let i = scopes.length - 1; i >= 0; i--) {
        const scope = scopes[i];
        if (scope.startsWith('keyword.operator.') || scope.startsWith('punctuation.')) return 2;
        if (scope.startsWith('keyword.control.') && scope.endsWith('.sdbl')) return 14;
        if (scope.startsWith('keyword.other.') || scope.startsWith('storage.modifier.directive.') ||
            scope.startsWith('entity.name.section.') || scope.startsWith('storage.type.annotation.')) return 8;
        if (scope.startsWith('keyword.') || scope.startsWith('storage.')) return 1;
        if (scope.startsWith('comment.')) return 3;
        if (scope.startsWith('string.') || scope.startsWith('constant.character.escape.')) return 4;
        if (scope.startsWith('constant.numeric.')) return 5;
        if (scope.startsWith('constant.other.date.')) return 6;
        if (scope.startsWith('constant.language.') || scope.startsWith('support.variable.')) return 7;
        if (scope.startsWith('entity.name.function.')) return 9;
        if (scope.startsWith('support.function.')) return query ? 15 : 10;
        if (scope.startsWith('variable.parameter.')) return query ? 16 : 11;
        if (scope.startsWith('variable.assignment.') || scope === 'variable.bsl') return 12;
        if (scope.startsWith('support.class.') || scope.startsWith('support.type.')) return query ? 17 : 13;
    }
    return 0;
}

let grammar;
async function initializeTextMate(bsl, sdbl) {
    const tm = globalThis.vscodetextmate;
    const grammars = {
        'source.bsl': tm.parseRawGrammar(bsl, '1c.tmLanguage.json'),
        'source.sdbl': tm.parseRawGrammar(sdbl, '1c-query.tmLanguage.json'),
    };
    // The upstream grammar recognizes the constructor itself. Preserve the
    // name following it as a distinct type scope in both BSL dialects.
    grammars['source.bsl'].repository.miscellaneous.patterns.unshift({
        match: '(?i:(?<![\\w.])(Новый|New)[ \\t]+([\\p{L}_][\\p{L}\\p{N}_]*))',
        captures: {
            '1': { name: 'storage.type.bsl' },
            '2': { name: 'support.type.bsl' },
        },
    });
    const registry = new tm.Registry({
        onigLib: Promise.resolve({
            createOnigScanner: nativeScanner,
            createOnigString: nativeString,
        }),
        loadGrammar: async name => grammars[name] ?? null,
    });
    grammar = await registry.loadGrammar('source.bsl');
}

function tokenizeModule(source) {
    if (!grammar) throw Error('TextMate grammar unavailable');
    let state = globalThis.vscodetextmate.INITIAL;
    return source.split(/\r\n|\r|\n/).map(line => {
        const result = grammar.tokenizeLine(line, state);
        state = result.ruleStack;
        const spans = [];
        for (const token of result.tokens) {
            const end = Math.min(line.length, token.endIndex);
            if (end <= token.startIndex) continue;
            const role = classify(token.scopes);
            const previous = spans.length - 3;
            if (previous >= 0 && spans[previous + 2] === role && spans[previous] + spans[previous + 1] === token.startIndex) {
                spans[previous + 1] = end - spans[previous];
            } else {
                spans.push(token.startIndex, end - token.startIndex, role);
            }
        }
        return spans;
    });
}
