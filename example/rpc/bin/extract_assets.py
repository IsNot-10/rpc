import re
import os

def extract_string(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Match return "...";
    match = re.search(r'return "(.*)";', content, re.DOTALL)
    if not match:
        return ""
    
    js_content = match.group(1)
    
    # Unescape C++ string literal
    # We need to handle \\ first, then \"
    # But wait, in C++, \\ represents a single backslash. \" represents a double quote.
    # The source file has e.g. "var x=\"hello\"" -> represented as "var x=\\\"hello\\\"" in C++ source?
    # No, usually minified JS uses single quotes or avoids quotes where possible.
    # But if it has quotes, they are escaped.
    
    # Let's verify with a small test case.
    # If file has: return "a\"b\\c"; -> string is: a"b\c
    # We want to put a"b\c into R"(...)"
    
    # Replace \" with "
    js_content = js_content.replace(r'\"', '"')
    # Replace \\ with \
    js_content = js_content.replace(r'\\', '\\')
    
    return js_content

jquery = extract_string('/home/abab/muduo-x/BRPC/src/brpc/builtin/jquery_min_js.cpp')
flot = extract_string('/home/abab/muduo-x/BRPC/src/brpc/builtin/flot_min_js.cpp')

with open('/home/abab/muduo-x/mymuduo/example/rpc/src/MonitorAssets.h', 'w') as f:
    f.write('#pragma once\n\n')
    f.write('const char* kJqueryMin = R"JQUERY(' + jquery + ')JQUERY";\n\n')
    f.write('const char* kFlotMin = R"FLOT(' + flot + ')FLOT";\n')

print("Assets extracted.")
