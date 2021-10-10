# Benchmark dependencies:
# pip install pi-rc522 spidev RPi.GPIO

from timeit import default_timer as timer
from pirc522 import RFID
from rc522pi import RC522

def use_rc522pi(dev):
    while not dev.ntag_try_select():
        pass
    dev.ntag_write(6, b'\xca\xfe\xb0\xba')
    data = list(dev.ntag_read(4))
    print(f'Tag {list(dev.tag_nfcid)}, page 6 contents: ' + ' '.join(map(hex, data[8:12])))

def use_pirc522(rdr):
    rdr.wait_for_tag()
    (error, _) = rdr.request()
    assert not error
    (error, uid) = rdr.anticoll("1")
    assert not error
    error = rdr.select_tag(uid, "1")
    assert not error
    (error, uid2) = rdr.anticoll("2")
    assert not error
    error = rdr.select_tag(uid2, "2")
    assert not error
    error = rdr.writentag(6, [0xDE, 0xAD, 0xBE, 0xEF])
    assert not error
    (error, data) = rdr.read(4)
    assert not error
    uid7 = uid[1:4] + uid2[0:4]
    print(f'Tag {uid7}, page 6 contents: ' + ' '.join(map(hex, data[8:12])))

def time_runs(n, f):
    timings = []
    for i in range(0, n):
        start = timer()
        f()
        end = timer()
        timings.append(end - start)
    return timings

pirc = RFID(speed=1_000_000, pin_rst=22)
pirc_timings = time_runs(10, lambda: use_pirc522(pirc))
pirc.cleanup()

rcpi = RC522(spi_baud_rate=1_000_000, antenna_gain=4, rst_pin=25)
rcpi_timings = time_runs(10, lambda: use_rc522pi(rcpi))

pirc_avg = sum(pirc_timings) / len(pirc_timings)
print(f'pirc522: avg: {pirc_avg}, runs: {[round(t, 3) for t in pirc_timings]}')
rcpi_avg = sum(rcpi_timings) / len(rcpi_timings)
print(f'rc522pi: avg: {rcpi_avg}, runs: {[round(t, 3) for t in rcpi_timings]}')

# Results on my RPi 3 Model B+:
# pirc522: avg: 0.07098794100000134, runs: [0.085, 0.07, 0.067, 0.066, 0.067, 0.067, 0.066, 0.083, 0.068, 0.069]
# rc522pi: avg: 0.03086293569999867, runs: [0.018, 0.032, 0.032, 0.032, 0.032, 0.032, 0.033, 0.032, 0.032, 0.034]
