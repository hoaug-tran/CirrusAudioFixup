"""Extract real method bodies across service/driver translation units for isolated tests.

The type-name substitution only supplies the existing mock primitive interface;
complete driver ownership is verified separately by check_cs35l41_backend.
"""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def source():
    service=(ROOT/'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text()
    driver=(ROOT/'CirrusAudioFixup/Devices/CS35L41/CS35L41Driver.cpp').read_text()
    # Omit facade overloads: isolation tests use the primitive hardware methods.
    driver=driver[:driver.index('CS35L41Driver::CS35L41Driver(')]
    return service+'\n'+driver.replace('CS35L41Driver::','CirrusAudioFixup::').replace('DriverRegisterIO','ServiceRegisterIO').replace('parseBootArgument(', 'PE_parse_boot_argn(')
def chip_state():
    s=(ROOT/'CirrusAudioFixup/Devices/CS35L41/DeviceState.hpp').read_text()
    a=s.index('struct DeviceState');b=s.index('\n};',a)+3
    return s[a:b].replace('struct DeviceState','struct AmplifierState')

def method(text, class_name, name):
    """Extract a method body with nested braces, comments and literals intact."""
    import re
    start=re.search(r'(?:bool|void|IOReturn|uint32_t|const char\*) '+re.escape(class_name)+r'::'+re.escape(name)+r'\(',text).start()
    opening=text.index('{',start)
    tokens=re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]',text[opening:])
    depth=0
    for token in tokens:
        if token.group()=='{':depth+=1
        elif token.group()=='}':
            depth-=1
            if depth==0:return text[start:opening+token.end()]
    raise ValueError('unterminated method '+name)
