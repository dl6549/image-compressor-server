const express = require('express');
const multer = require('multer');
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const cors = require('cors');

const app = express();
const PORT = process.env.PORT || 3000;

app.use(cors());
app.use(express.json());
app.use(express.static('public'));

const uploadsDir = path.join(__dirname, 'uploads');
const outputsDir = path.join(__dirname, 'outputs');

if (!fs.existsSync(uploadsDir)) {
    fs.mkdirSync(uploadsDir);
}
if (!fs.existsSync(outputsDir)) {
    fs.mkdirSync(outputsDir);
}

const storage = multer.diskStorage({
    destination: function (req, file, cb) {
        cb(null, uploadsDir);
    },
    filename: function (req, file, cb) {
        const uniqueSuffix = Date.now() + '-' + Math.round(Math.random() * 1E9);
        cb(null, uniqueSuffix + path.extname(file.originalname));
    }
});

const upload = multer({
    storage: storage,
    limits: { fileSize: 50 * 1024 * 1024 },
    fileFilter: function (req, file, cb) {
        const allowedTypes = /jpeg|jpg|png/;
        const extname = allowedTypes.test(path.extname(file.originalname).toLowerCase());
        const mimetype = allowedTypes.test(file.mimetype);

        if (mimetype && extname) {
            return cb(null, true);
        } else {
            cb(new Error('Only PNG, JPG, and JPEG images are allowed!'));
        }
    }
});

app.post('/compress', upload.single('image'), async (req, res) => {
    if (!req.file) {
        return res.status(400).json({ error: 'No file uploaded' });
    }

    const quality = parseFloat(req.body.quality) || 0.8;
    const format = req.body.format || 'jpg';

    if (quality < 0 || quality > 1) {
        return res.status(400).json({ error: 'Quality must be between 0 and 1' });
    }

    if (!['jpg', 'jpeg', 'png'].includes(format.toLowerCase())) {
        return res.status(400).json({ error: 'Format must be jpg or png' });
    }

    const inputPath = req.file.path;
    const outputFilename = `compressed-${Date.now()}.${format}`;
    const outputPath = path.join(outputsDir, outputFilename);

    const isWindows = process.platform === 'win32';
    const compressorPath = path.join(__dirname, isWindows ? 'compress.exe' : 'compress');

    console.log('=== Compression Request ===');
    console.log('Platform:', process.platform);
    console.log('Compressor path:', compressorPath);
    console.log('Input:', inputPath);
    console.log('Output:', outputPath);
    console.log('Quality:', quality);
    console.log('Format:', format);

    if (!fs.existsSync(compressorPath)) {
        console.error('Compressor not found at:', compressorPath);
        fs.unlinkSync(inputPath);
        return res.status(500).json({ 
            error: 'Compression binary not found on server',
            path: compressorPath,
            platform: process.platform
        });
    }

    console.log('Compressor found, starting compression...');

    const compressProcess = spawn(compressorPath, [inputPath, outputPath, quality.toString()]);

    let stdout = '';
    let stderr = '';

    compressProcess.stdout.on('data', (data) => {
        stdout += data.toString();
        console.log('[C++]:', data.toString());
    });

    compressProcess.stderr.on('data', (data) => {
        stderr += data.toString();
        console.error('[C++ Error]:', data.toString());
    });

    compressProcess.on('close', (code) => {
        console.log('C++ process exited with code:', code);

        try {
            fs.unlinkSync(inputPath);
        } catch (err) {
            console.error('Error deleting input file:', err);
        }

        if (code === 0) {
            if (fs.existsSync(outputPath)) {
                const inputSize = req.file.size;
                const outputSize = fs.statSync(outputPath).size;
                const reduction = (((inputSize - outputSize) / inputSize) * 100).toFixed(1);

                console.log('✓ Compression successful');
                console.log('Input size:', inputSize, 'bytes');
                console.log('Output size:', outputSize, 'bytes');
                console.log('Reduction:', reduction + '%');

                res.json({
                    success: true,
                    filename: outputFilename,
                    downloadUrl: `/download/${outputFilename}`,
                    originalSize: inputSize,
                    compressedSize: outputSize,
                    reduction: reduction,
                    log: stdout
                });

                setTimeout(() => {
                    try {
                        if (fs.existsSync(outputPath)) {
                            fs.unlinkSync(outputPath);
                            console.log('Cleaned up:', outputFilename);
                        }
                    } catch (err) {
                        console.error('Error cleaning up output file:', err);
                    }
                }, 10 * 60 * 1000);

            } else {
                res.status(500).json({
                    success: false,
                    error: 'Compression completed but output file not found',
                    log: stdout
                });
            }
        } else {
            res.status(500).json({
                success: false,
                error: stderr || 'Compression failed',
                log: stdout,
                exitCode: code
            });
        }
    });

    compressProcess.on('error', (err) => {
        console.error('Failed to start compress process:', err);
        
        try {
            fs.unlinkSync(inputPath);
        } catch (e) {
            console.error('Error deleting input file:', e);
        }

        res.status(500).json({
            success: false,
            error: 'Failed to start compression process: ' + err.message
        });
    });
});

app.get('/download/:filename', (req, res) => {
    const filename = req.params.filename;
    const filepath = path.join(outputsDir, filename);

    if (fs.existsSync(filepath)) {
        res.download(filepath, filename, (err) => {
            if (err) {
                console.error('Error sending file:', err);
            }
        });
    } else {
        res.status(404).json({ error: 'File not found or expired' });
    }
});

app.get('/health', (req, res) => {
    const isWindows = process.platform === 'win32';
    const compressorPath = path.join(__dirname, isWindows ? 'compress.exe' : 'compress');
    const compressorExists = fs.existsSync(compressorPath);
    
    res.json({ 
        status: 'ok',
        platform: process.platform,
        compressorPath: compressorPath,
        compressorExists: compressorExists,
        nodeVersion: process.version,
        uptime: process.uptime()
    });
});

app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

app.listen(PORT, () => {
    const isWindows = process.platform === 'win32';
    const compressorPath = path.join(__dirname, isWindows ? 'compress.exe' : 'compress');
    const compressorExists = fs.existsSync(compressorPath);
    
    console.log('=================================');
    console.log(`🚀 Image Compressor Server`);
    console.log(`📍 Running on: http://localhost:${PORT}`);
    console.log(`🖥️  Platform: ${process.platform}`);
    console.log(`📦 Compressor: ${compressorExists ? '✓ Found' : '✗ Not Found'}`);
    console.log(`   Path: ${compressorPath}`);
    console.log('=================================');
});