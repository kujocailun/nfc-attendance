import argparse
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0 , str(Path(__file__).resolve().parents[1]))
from network_server import Settings


def sol():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--issuer" , required = True)
    parser.add_argument("--audience" , required = True)
    parser.add_argument("--admin-host" , required = True)
    parser.add_argument("--device-host" , required = True)
    parser.add_argument("--admin-email" , required = True)
    args = parser.parse_args()
    settings = Settings(admin_host = args.admin_host , device_host = args.device_host , admin_email = args.admin_email ,
                        access_issuer = args.issuer.rstrip("/") , access_audience = args.audience , local_admin = False)
    settings.validate()
    target = root / "network-config.json"
    (root / "deploy-state").mkdir(exist_ok = True)
    if target.exists():
        shutil.copy2(target , root / "deploy-state" / "previous-config.json")
    target.write_text(json.dumps(vars(settings) , ensure_ascii = False , indent = 4) + "\n" , encoding = "utf-8")
    print("Production configuration saved. Local admin bypass is disabled.")


if __name__ == "__main__":
    sol()
