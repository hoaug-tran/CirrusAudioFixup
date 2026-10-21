"""Read the ASCII property-list syntax used by Xcode project files."""
import json
import re


def parse_project(text):
    tokens = re.findall(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|[{}()=;,]|[^\s{}()=;,"]+', text)
    tokens = iter(token for token in tokens if not token.startswith(('//', '/*')))

    def value(token):
        if token == '{':
            result = {}
            while (key := next(tokens)) != '}':
                assert next(tokens) == '='
                result[json.loads(key) if key.startswith('"') else key] = value(next(tokens))
                assert next(tokens) == ';'
            return result
        if token == '(':
            result = []
            while (item := next(tokens)) != ')':
                result.append(value(item))
                separator = next(tokens)
                if separator == ')':
                    return result
                assert separator == ','
            return result
        return json.loads(token) if token.startswith('"') else token

    result = value(next(tokens))
    assert next(tokens, None) is None
    return result
