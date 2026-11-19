"""Feed real sensor history to the guest SDK's FullKey motion reader."""
from pathlib import Path
import shutil

SOURCE = Path(__file__).resolve().parents[1]/'runtime/mk8'


def prepare(runtime):
    input_path = runtime/'hle/input.c'
    hid_path = runtime/'services/hid_set.c'
    inputs = input_path.read_text(encoding='utf-8')
    hid = hid_path.read_text(encoding='utf-8')
    implementation = (SOURCE/'services/motion.inc').read_text(encoding='utf-8')
    read_symbol = '_ZN2nn3hid22GetSixAxisSensorStates'
    service = 'SERVICE("hid", hid) {'
    old = '    case 101: OUT(uint32_t, 0) = 0x1F;'
    if (read_symbol in inputs or inputs.count('#define EXTRA_PADS 3') != 1
            or hid.count(service) != 1 or hid.count(old) != 1
            or 'case 66:' in hid or 'case 67:' in hid):
        raise ValueError('FullKey six-axis input boundary changed upstream')
    hid = hid.replace(service, 'extern uint32_t mk8_motion_control(const uint8_t *, size_t, bool);\n\n' + service)
    hid = hid.replace(old, '''    case 66: case 67:  // StartSixAxisSensor / StopSixAxisSensor
        resp->result = mk8_motion_control(req->data, req->data_size, req->cmd == 66);
        break;
''' + old)
    shutil.copyfile(SOURCE/'support/motion_pose.h', runtime/'hle/mk8_motion_pose.h')
    input_path.write_text(inputs + '\n' + implementation + '\n', encoding='utf-8')
    hid_path.write_text(hid, encoding='utf-8')
