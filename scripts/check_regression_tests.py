import argparse
import glob
import sys
import os
import re
import hashlib

from pathlib import Path

FILE_HEADER = """
<!DOCTYPE html>
<html>
<head>
<title>Comparison</title>
<style>
.modal {
  display: none;
  position: fixed;
  z-index: 1;
  padding-top: 100px;
  left: 0;
  top: 0;
  width: 100%;
  height: 100%;
  overflow: auto;
  background-color: black;
}

.modal-content {
  position: relative;
  margin: auto;
  padding: 0;
  width: 90%;
  max-width: 1200px;
}

.close {
  color: white;
  position: absolute;
  top: 10px;
  right: 25px;
  font-size: 35px;
  font-weight: bold;
}

.close:hover,
.close:focus {
  color: #999;
  text-decoration: none;
  cursor: pointer;
}

.prev,
.next {
  cursor: pointer;
  position: absolute;
  top: 50%;
  width: auto;
  padding: 16px;
  margin-top: -50px;
  color: white;
  font-weight: bold;
  font-size: 20px;
  transition: 0.6s ease;
  border-radius: 0 3px 3px 0;
  user-select: none;
  -webkit-user-select: none;
}

.next {
  right: 0;
  border-radius: 3px 0 0 3px;
}

.prev:hover,
.next:hover {
  background-color: rgba(0, 0, 0, 0.8);
}

.item img {
    cursor: pointer;
}

#compareTitle {
    color: white;
    font-size: 20px;
    font-family: sans-serif;
    margin-bottom: 10px;
}

#compareCaption {
    color: white;
    font-family: sans-serif;
}

#compareState {
    display: block;
    position: absolute;
    right: 0;
    top: 0;
    color: red;
    font-family: sans-serif;
    font-size: 20px;
    font-weight: bold;
}

#compareImage {
    display: block;
    margin: 0 auto;
    width: 100%;
    height: auto;
}
</style>
</head>
<body>
"""

FILE_FOOTER = """
<div id="myModal" class="modal" tabindex="0">
  <span class="close cursor" onclick="closeModal()">&times;</span>
  <div class="modal-content">
    <div id="compareTitle"></div>
    <img id="compareImage" />
    <div id="compareState"></div>
    <a class="prev">&#10094;</a>
    <a class="next">&#10095;</a>
    <div id="compareCaption"></div>
  </div>
</div>
<script>
/* Worst script known to man */
/* Sources:
   https://www.w3schools.com/howto/howto_js_lightbox.asp
   https://css-tricks.com/prevent-page-scrolling-when-a-modal-is-open/
*/

function openModal() {
  document.body.style.position = 'fixed';
  document.body.style.top = `-${window.scrollY}px`;
  document.getElementById("myModal").style.display = "block";
  document.getElementById("myModal").focus();
  setImageIndex(0);
}

function closeModal() {
    document.getElementById("myModal").style.display = "none";
    const scrollY = document.body.style.top;
    document.body.style.position = '';
    document.body.style.top = '';
    window.scrollTo(0, parseInt(scrollY || '0') * -1);
}

function isModalOpen() {
    return (document.getElementById("myModal").style.display == "block");
}

function formatLines(str) {
    let lines = str.split("\\n")
    lines = lines.filter(line => !line.startsWith("Difference in frames"))
    return lines.join("<br>")
}

function extractItem(elem) {
    var beforeSel = elem.querySelector(".before");
    var afterSel = elem.querySelector(".after");
    var preSel = elem.querySelector("pre");
    return {
        name: elem.querySelector("h1").innerText,
        beforeImg: beforeSel ? beforeSel.getAttribute("src") : null,
        afterImg: afterSel ? afterSel.getAttribute("src") : null,
        details: formatLines(preSel ? preSel.innerText : "")
    };
}

const items = [...document.querySelectorAll(".item")].map(extractItem)
let currentImage = 0;
let currentState = 0;

function getImageIndexForUri(uri) {
    for (let i = 0; i < items.length; i++) {
        if (items[i].beforeImg == uri || items[i].afterImg == uri)
            return i;
    }
    return -1;
}

function setImageState(state) {
    const item = items[currentImage]
    const uri = (state === 0) ? item.beforeImg : item.afterImg;
    const stateText = (state === 0) ? "BEFORE" : "AFTER";
    const posText = "(" + (currentImage + 1).toString() + "/" + (items.length).toString() + ") ";
    document.getElementById("compareImage").setAttribute("src", uri);
    document.getElementById("compareState").innerText = stateText;
    document.getElementById("compareTitle").innerText = posText + item.name;
    document.getElementById("compareCaption").innerHTML = item.details;
    currentState = state;
}

function setImageIndex(index) {
    if (index < 0 || index > items.length)
        return;

    currentImage = index;
    setImageState(0);
}

function handleKey(key) {
    if (key == " ") {
        setImageState((currentState === 0) ? 1 : 0);
        return true;
    } else if (key == "ArrowLeft") {
        setImageIndex(currentImage - 1);
        return true;
    } else if (key == "ArrowRight") {
        setImageIndex(currentImage + 1);
        return true;
    } else if (key == "Escape") {
        closeModal();
        return true;
    } else {
        console.log(key);
        return false;
    }
}

document.getElementById("myModal").addEventListener("keydown", function(ev) {
    if (ev.defaultPrevented)
        return;
    
    if (handleKey(ev.key))
        ev.preventDefault();
});

document.querySelector("#myModal .prev").addEventListener("click", function() {
    setImageIndex(currentImage - 1);
});
document.querySelector("#myModal .next").addEventListener("click", function() {
    setImageIndex(currentImage + 1);
});
document.querySelectorAll(".item img").forEach(elem => elem.addEventListener("click", function() {
    if (!isModalOpen())
        openModal();
    setImageIndex(getImageIndexForUri(this.getAttribute("src")));
}));
</script>
</body>
</html>
"""

MAX_DIFF_FRAMES = 9999

outfile = None
def write(line):
    outfile.write(line + "\n")


def compare_frames(path1, path2):
    try:
        with open(path1, "rb") as f:
            hash1 = hashlib.md5(f.read()).digest()
        with open(path2, "rb") as f:
            hash2 = hashlib.md5(f.read()).digest()

        #print(hash1, hash2)
        return hash1 == hash2
    except:
        return False


def check_regression_test(baselinedir, testdir, name):
    #print("Checking '%s'..." % name)

    dir1 = os.path.join(baselinedir, name)
    dir2 = os.path.join(testdir, name)
    if not os.path.isdir(dir2):
        #print("*** %s is missing in test set" % name)
        return False

    images = glob.glob(os.path.join(dir1, "frame_*.png"))
    diff_frames = []
    first_fail = True
    has_any = False
    
    for imagepath in images:
        imagename = Path(imagepath).name
        matches = re.match("frame_([0-9]+).png", imagename)
        if matches is None:
            continue

        framenum = int(matches[1])

        path1 = os.path.join(dir1, imagename)
        path2 = os.path.join(dir2, imagename)
        if not os.path.isfile(path2):
            print("--- Frame %u for %s is missing in test set" % (framenum, name))
            if first_fail:
                write("<div class=\"item\">")
                write("<h1>{}</h1>".format(name))
                write("<table width=\"100%\">")
                first_fail = False
            write("<p>--- Frame %u for %s is missing in test set</p>" % (framenum, name))
            continue

        has_any = True
        if not compare_frames(path1, path2):
            diff_frames.append(framenum)

            if first_fail:
                write("<div class=\"item\">")
                write("<h1>{}</h1>".format(name))
                write("<table width=\"100%\">")
                first_fail = False

            imguri1 = Path(path1).as_uri()
            imguri2 = Path(path2).as_uri()
            write("<tr><td colspan=\"2\">Frame %d</td></tr>" % (framenum))
            write("<tr><td><img class=\"before\" src=\"%s\" /></td><td><img class=\"after\" src=\"%s\" /></td></tr>" % (imguri1, imguri2))

            if len(diff_frames) == MAX_DIFF_FRAMES:
                break

    if not first_fail:
        write("</table>")
        write("<pre>Difference in frames [%s] for %s</pre>" % (",".join(map(str, diff_frames)), name))
        write("</div>")
        print("*** Difference in frames [%s] for %s" % (",".join(map(str, diff_frames)), name))
        #assert has_any

    return len(diff_frames) == 0


def check_regression_tests(baselinedir, testdir):
    gamedirs = glob.glob(baselinedir + "/*", recursive=False)
    
    success = 0
    failure = 0

    for gamedir in gamedirs:
        name = Path(gamedir).name
        if check_regression_test(baselinedir, testdir, name):
            success += 1
        else:
            failure += 1

    return (failure == 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Check frame dump images for regression tests")
    parser.add_argument("-baselinedir", action="store", required=True, help="Directory containing baseline frames to check against")
    parser.add_argument("-testdir", action="store", required=True, help="Directory containing frames to check")
    parser.add_argument("-maxframes", type=int, action="store", required=False, default=9999, help="Max frames to compare")
    parser.add_argument("outfile", action="store", help="The file to write the output to")

    args = parser.parse_args()
    MAX_DIFF_FRAMES = args.maxframes

    outfile = open(args.outfile, "w")
    write(FILE_HEADER)

    if not check_regression_tests(os.path.realpath(args.baselinedir), os.path.realpath(args.testdir)):
        write(FILE_FOOTER)
        outfile.close()
        sys.exit(1)
    else:
        outfile.close()
        os.remove(args.outfile)
        sys.exit(0)

