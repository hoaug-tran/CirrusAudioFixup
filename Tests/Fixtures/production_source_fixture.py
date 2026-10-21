"""Extract real method bodies across service/driver translation units for isolated tests.

The type-name substitution only supplies the existing mock primitive interface;
complete driver ownership is verified separately by check_cs35l41_driver.
"""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SERVICE_SOURCES = ("CirrusAudioFixup", "CirrusAudioFixupAttachment", "CirrusAudioFixupRuntime", "CirrusAudioFixupPower", "CirrusAudioFixupDiagnostics")
DRIVER_SOURCES = ("Hardware", "Firmware", "Playback", "Diagnostics", "Driver")

def service_source():
    directory = ROOT / "CirrusAudioFixup/Service"
    return (directory / "CirrusAudioFixupSupport.hpp").read_text() + "\n" + "\n".join(
        (directory / (name + ".cpp")).read_text() for name in SERVICE_SOURCES)

def driver_source():
    directory = ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L41"
    return (directory / "CS35L41Internal.hpp").read_text().replace("inline uint32_t", "static uint32_t") + "\n" + "\n".join(
        (directory / ("CS35L41" + name + ".cpp")).read_text() for name in DRIVER_SOURCES)

def driver_sources():
    directory = ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L41"
    return [directory / ("CS35L41" + name + ".cpp") for name in DRIVER_SOURCES] + [directory / "Resources/FirmwareData.cpp"]

def source():
    service = service_source()
    driver = driver_source()
    driver = driver[:driver.index('CS35L41Driver::CS35L41Driver(')]
    return service+'\n'+driver.replace('CS35L41Driver::','CirrusAudioFixup::').replace('DriverRegisterIO','ServiceRegisterIO').replace('parseBootArgument(', 'PE_parse_boot_argn(')

def chip_state():
    s=(ROOT/'CirrusAudioFixup/Devices/Amplifiers/CS35L41/InstanceState.hpp').read_text()
    a=s.index('struct InstanceState');b=s.index('\n};',a)+3
    return s[a:b].replace('struct InstanceState','struct AmplifierState')

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
