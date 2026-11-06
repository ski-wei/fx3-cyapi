import os
import time
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtWidgets import QApplication
from fx3_cyapi.gui import MainWindow


def wait_for(app, predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        app.processEvents()
        if predicate():
            return
        time.sleep(0.005)
    raise AssertionError("Qt command did not complete")


def test_simulated_gui_start_preview_and_async_close():
    app = QApplication.instance() or QApplication([])
    window = MainWindow()
    window.block_kib.setValue(4)
    window.show()
    window.start()
    wait_for(app, lambda: window.job is None and window.engine.stats().running)
    wait_for(app, lambda: len(window.preview.values) > 0)
    assert window.preview.values.size <= 1024
    window.close()
    wait_for(app, lambda: window.close_ready and window.job is None)
    assert not window.engine.stats().opened
    assert not window.isVisible()


def test_window_close_while_starting():
    app = QApplication.instance() or QApplication([])
    window = MainWindow()
    window.block_kib.setValue(4)
    window.show()
    window.start()
    window.close()
    wait_for(app, lambda: window.close_ready and window.job is None)
    assert not window.engine.stats().opened
