import unittest
import time
import subprocess
from lib.bank_client import *


class RecoveryTest(unittest.TestCase):
    def setUp(self):
        #subprocess.check_call(['blockade','up'])
        self.clients = ClientCollection([
            "dbname=postgres host=127.0.0.1 user=postgres",
            "dbname=postgres host=127.0.0.1 user=postgres port=5433",
            "dbname=postgres host=127.0.0.1 user=postgres port=5434"
        ])
        self.clients.start()

    def tearDown(self):
        self.clients.stop()
        subprocess.check_call(['blockade','join'])

    def test_0_normal_operation(self):
        print('### normalOpsTest ###')
        print('Waiting 5s to check operability')
        time.sleep(5)

        for client in self.clients:
            agg = client.history.aggregate()
            print(agg)
            # naively check that we did at least some set ops
            self.assertTrue(agg['setkey']['commit'] > 10)

    def test_1_node_disconnect(self):
        print('### disconnectTest ###')

        subprocess.check_call(['blockade','partition','node3'])
        print('Node3 disconnected')

        print('Waiting 5s to discover failure')
        time.sleep(5)

        for client in self.clients:
            agg = client.history.aggregate()
            print(agg)
            # check we didn't stuck in set op
            self.assertTrue(agg['setkey']['running_latency'] < 3)

if __name__ == '__main__':
    unittest.main()



